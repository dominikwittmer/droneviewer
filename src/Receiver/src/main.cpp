/*
 * node-mode with dual Wi-Fi and BLE support for ESP32S3
 * INJECT <mac> <lat> <lon> <alt_m> <heading_deg> <speed_mps> [base_lat] [base_lon] [uav_id]
 */
#if !defined(ARDUINO_ARCH_ESP32)
#error "This program requires an ESP32"
#endif

#include <Arduino.h>
#include <HardwareSerial.h>
#include <NimBLEDevice.h>
#include <NimBLEUtils.h>
#include <NimBLEScan.h>
#include <WiFi.h>
#include <esp_wifi.h>
#include <Preferences.h>
#include <nvs_flash.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include "opendroneid.h"
#include "odid_wifi.h"
#include <esp_timer.h>
#include <esp_sleep.h>
#include <driver/rtc_io.h>
#include <Wire.h>
#include <Adafruit_MAX1704X.h>
#include <Adafruit_NeoPixel.h>
#include <esp_task_wdt.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <ArduinoJson.h>

// Structure to hold UAV detection data
struct uav_data
{
  uint8_t mac[6];
  int rssi;
  uint32_t last_seen;
  char op_id[ODID_ID_SIZE + 1];
  char uav_id[ODID_ID_SIZE + 1];
  double lat_d;
  double long_d;
  double base_lat_d;
  double base_long_d;
  int altitude_msl;
  int height_agl;
  int speed;
  int heading;
};

#define MAX_UAVS 40
#define UAV_EVENT_QUEUE_LENGTH 512

#define JSON_DOC_SIZE 512
#define WATCHDOG_TIMEOUT_SEC 10
#ifndef WIFI_CALLBACK_TRACE
#define WIFI_CALLBACK_TRACE 0
#endif

#define SAFE_STRCPY(dst, src)               \
  do                                        \
  {                                         \
    strncpy((dst), (src), sizeof(dst) - 1); \
    (dst)[sizeof(dst) - 1] = '\0';          \
  } while (0)

typedef enum
{
  APP_ERR_OK = 0,
  APP_ERR_QUEUE_FULL,
  APP_ERR_NULL_POINTER,
  APP_ERR_BLE_NOTIFY,
  APP_ERR_WIFI_PARSE,
  APP_ERR_SERIAL_PARSE,
  APP_ERR_JSON_OVERFLOW,
  APP_ERR_BATTERY_SENSOR,
  APP_ERR_MUTEX_TIMEOUT,
  APP_ERR_UNKNOWN
} ErrorCode;

#define HANDLE_ERROR(x) errorHandler(x, __FILE__, __LINE__)

enum uav_source_t : uint8_t
{
  UAV_SOURCE_BLE = 1,
  UAV_SOURCE_WIFI = 2,
  UAV_SOURCE_SERIAL = 3,
};

struct uav_event
{
  uav_data data;
  uav_source_t source;
  bool hasBasicId;
  bool hasLocation;
  bool hasSystem;
  bool hasOperatorId;
};

static uav_data uavs[MAX_UAVS] = {};
static BLEScan *pBLEScan = nullptr;
static NimBLECharacteristic *pTelemetryCharacteristic = nullptr;
static NimBLECharacteristic *pBatteryLevelCharacteristic = nullptr;
static NimBLECharacteristic *pBatteryChargeStateCharacteristic = nullptr;
static SemaphoreHandle_t bleTxMutex = nullptr;
static QueueHandle_t uavEventQueue = nullptr;
static volatile bool bleClientConnected = false;
static ODID_UAS_Data UAS_data;
static unsigned long last_status = 0;
static unsigned long last_drone_seen = 0;
static unsigned long last_ble_heartbeat = 0;
static unsigned long last_pixel_idle_blink = 0;
static unsigned long pixel_off_at = 0;
static unsigned long data_signal_until = 0;
static Adafruit_MAX17048 maxlipo;
static unsigned long last_battery_update = 0;
static Preferences blePrefs;
static uint32_t blePasskey = 123456;
static unsigned long buttonPressStart = 0;
static unsigned long buttonLastEdgeAt = 0;
static int buttonRawState = HIGH;
static int buttonDebouncedState = HIGH;
static bool buttonWasPressed = false;
static bool requireButtonReleaseAfterWake = false;
static bool buttonSleepArmed = false;
static const uint32_t RID_MIN_INTERVAL_MS = 1000;
static bool serialJsonForInjectedEnabled = false;
static bool serialInjectAckEnabled = false;
static volatile uint32_t uavQueueHighWatermark = 0;
static volatile uint32_t uavQueueEnqueueCount = 0;
static volatile uint32_t uavQueueDequeueCount = 0;
static volatile uint32_t uavQueueFullCount = 0;
static volatile uint32_t bleNotifyAttemptCount = 0;
static volatile uint32_t bleNotifySentCount = 0;
static volatile uint32_t bleNotifyNoClientCount = 0;
static volatile uint32_t bleNotifyNoSubCount = 0;
static volatile uint32_t bleNotifyMutexTimeoutCount = 0;
static volatile uint32_t bleNotifyErrorCount = 0;
static volatile uint32_t bleNotifyChunkFailCount = 0;
static uint8_t throttleMacs[MAX_UAVS][6] = {};
static uint32_t throttleLastMs[MAX_UAVS] = {};
static bool throttleUsed[MAX_UAVS] = {};

#ifndef LED_BUILTIN
#define LED_BUILTIN 21
#endif

#ifndef NEOPIXEL_PIN
#ifdef PIN_NEOPIXEL
#define NEOPIXEL_PIN PIN_NEOPIXEL
#else
#define NEOPIXEL_PIN 8
#endif
#endif

static const char *BLE_SERVICE_UUID = "12345678-1234-1234-1234-1234567890ab";
static const char *BLE_TELEMETRY_UUID = "12345678-1234-1234-1234-1234567890ac";
static const char *BLE_BATTERY_CHARGE_STATE_UUID = "12345678-1234-1234-1234-1234567890ad";
static const uint32_t BLE_PASSKEY_DEFAULT = 123456;
static const char *BLE_PREF_NAMESPACE = "blecfg";
static const char *BLE_PREF_PASSKEY = "passkey";
static const gpio_num_t BUTTON_PIN = GPIO_NUM_10;
static const unsigned long BUTTON_DEBOUNCE_MS = 40;
static const unsigned long BUTTON_HOLD_TIME_MS = 5000;
static const float BATTERY_CHARGE_STATE_DEADBAND_PCT_PER_H = 0.5f;

enum battery_charge_state_t : uint8_t
{
  BATTERY_STATE_DISCHARGING = 0,
  BATTERY_STATE_CHARGING = 1,
  BATTERY_STATE_IDLE = 2,
};

// Forward declarations
static void callback(void *, wifi_promiscuous_pkt_type_t);
static void send_json_fast(const uav_data *UAV, const char *extra_fields = "", bool emitSerial = true);
static void send_ble_notification(const char *message);
static void send_waypoint_notifications(const uav_data *UAV, const char *source);
static void refresh_ble_whitelist_filter();
static void process_serial_commands();
static void handle_serial_command(const String &cmd);
static bool set_ble_passkey(uint32_t newPasskey);
static bool enqueue_uav_event(const uav_data *data, uav_source_t source, bool hasBasicId, bool hasLocation, bool hasSystem, bool hasOperatorId, TickType_t waitTicks = 0);
static const char *source_extra_fields(uav_source_t source);
static bool parse_serial_injected_uav(const String &payload, uav_data *out);
static void signal_low_battery_blink();
static void merge_uav_event(uav_data *target, const uav_event *event);
static bool has_plausible_location(const uav_data *data);
static void update_uav_queue_metrics();
static void print_uav_queue_status();
static void print_ble_notify_status();
static void checkButtonForDeepSleep();

static bool allow_uav_1hz(const uint8_t mac[6], uint32_t nowMs)
{
  int freeIndex = -1;
  for (int i = 0; i < MAX_UAVS; i++)
  {
    if (throttleUsed[i] && memcmp(throttleMacs[i], mac, 6) == 0)
    {
      if ((uint32_t)(nowMs - throttleLastMs[i]) < RID_MIN_INTERVAL_MS)
        return false;
      throttleLastMs[i] = nowMs;
      return true;
    }
    if (!throttleUsed[i] && freeIndex < 0)
      freeIndex = i;
  }

  if (freeIndex < 0)
    freeIndex = 0;
  throttleUsed[freeIndex] = true;
  memcpy(throttleMacs[freeIndex], mac, 6);
  throttleLastMs[freeIndex] = nowMs;
  return true;
}

static void errorHandler(ErrorCode err, const char *file, int line)
{
  Serial.printf(
      "[ERROR] code=%d file=%s line=%d heap=%u stack=%u\n",
      err,
      file,
      line,
      ESP.getFreeHeap(),
      uxTaskGetStackHighWaterMark(NULL));

  signal_low_battery_blink();
}

Adafruit_NeoPixel statusPixel(1, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);

static void set_status_pixel(uint8_t r, uint8_t g, uint8_t b, uint32_t duration_ms = 0)
{
  statusPixel.setPixelColor(0, statusPixel.Color(r, g, b));
  statusPixel.show();
  if (duration_ms > 0)
  {
    pixel_off_at = millis() + duration_ms;
  }
  else
  {
    pixel_off_at = 0;
  }
}

static void clear_status_pixel()
{
  statusPixel.setPixelColor(0, 0);
  statusPixel.show();
  pixel_off_at = 0;
}

static void signal_data_received()
{
  set_status_pixel(0, 0, 255, 1000);
  data_signal_until = millis() + 250;
}

static void signal_idle_blink()
{
  if (millis() < data_signal_until)
  {
    return;
  }
  set_status_pixel(0, 255, 0, 250);
}

static void signal_low_battery_blink()
{
  if (millis() < data_signal_until)
  {
    return;
  }
  set_status_pixel(255, 0, 0, 250);
}

static void update_status_pixel(unsigned long current_millis)
{
  if (pixel_off_at > 0 && current_millis >= pixel_off_at)
  {
    clear_status_pixel();
    pixel_off_at = 0;
  }

  if (last_drone_seen > 0 && (current_millis - last_drone_seen) >= 30000UL &&
      (current_millis - last_pixel_idle_blink) >= 30000UL)
  {
    signal_idle_blink();
    last_pixel_idle_blink = current_millis;
  }
}

static void print_heap_status()
{
  Serial.printf(
      "[HEAP] free=%u largest=%u min=%u\n",
      ESP.getFreeHeap(),
      heap_caps_get_largest_free_block(MALLOC_CAP_8BIT),
      esp_get_minimum_free_heap_size());
}

static void update_uav_queue_metrics()
{
  if (uavEventQueue == nullptr)
  {
    return;
  }

  UBaseType_t queued = uxQueueMessagesWaiting(uavEventQueue);
  if (queued > uavQueueHighWatermark)
  {
    uavQueueHighWatermark = queued;
  }
}

static void print_uav_queue_status()
{
  if (uavEventQueue == nullptr)
  {
    Serial.println("[QUEUE] unavailable");
    return;
  }

  UBaseType_t queued = uxQueueMessagesWaiting(uavEventQueue);
  UBaseType_t spaces = uxQueueSpacesAvailable(uavEventQueue);
  Serial.printf(
      "[QUEUE] queued=%u spaces=%u high=%lu enq=%lu deq=%lu full=%lu\n",
      (unsigned int)queued,
      (unsigned int)spaces,
      (unsigned long)uavQueueHighWatermark,
      (unsigned long)uavQueueEnqueueCount,
      (unsigned long)uavQueueDequeueCount,
      (unsigned long)uavQueueFullCount);
}

static void print_ble_notify_status()
{
  uint32_t subscribed = 0;
  if (pTelemetryCharacteristic != nullptr)
  {
    subscribed = (uint32_t)pTelemetryCharacteristic->getSubscribedCount();
  }

  Serial.printf(
      "[BLE] connected=%u subs=%lu attempt=%lu sent=%lu no_client=%lu no_sub=%lu mutex_to=%lu err=%lu chunk_fail=%lu\n",
      bleClientConnected ? 1 : 0,
      (unsigned long)subscribed,
      (unsigned long)bleNotifyAttemptCount,
      (unsigned long)bleNotifySentCount,
      (unsigned long)bleNotifyNoClientCount,
      (unsigned long)bleNotifyNoSubCount,
      (unsigned long)bleNotifyMutexTimeoutCount,
      (unsigned long)bleNotifyErrorCount,
      (unsigned long)bleNotifyChunkFailCount);
}

static bool enqueue_uav_event(const uav_data *data, uav_source_t source, bool hasBasicId, bool hasLocation, bool hasSystem, bool hasOperatorId, TickType_t waitTicks)
{
  if (uavEventQueue == nullptr || data == nullptr)
  {
    HANDLE_ERROR(APP_ERR_NULL_POINTER);
    return false;
  }

  uav_event event;
  memset(&event, 0, sizeof(event));
  memcpy(&event.data, data, sizeof(uav_data));
  event.source = source;
  event.hasBasicId = hasBasicId;
  event.hasLocation = hasLocation;
  event.hasSystem = hasSystem;
  event.hasOperatorId = hasOperatorId;

  if (xQueueSend(uavEventQueue, &event, waitTicks) != pdTRUE)
  {
    uavQueueFullCount++;
    update_uav_queue_metrics();
    return false;
  }

  uavQueueEnqueueCount++;
  update_uav_queue_metrics();

  return true;
}

static const char *source_extra_fields(uav_source_t source)
{
  if (source == UAV_SOURCE_BLE)
  {
    return ",\"source\":\"BLE\"}";
  }

  if (source == UAV_SOURCE_WIFI)
  {
    return ",\"source\":\"WiFi\"}";
  }

  if (source == UAV_SOURCE_SERIAL)
  {
    return ",\"source\":\"Serial\"}";
  }

  return ",\"source\":\"UNKNOWN\"}";
}

static bool parse_hex_byte(const char *token, uint8_t *out)
{
  if (token == nullptr || out == nullptr)
  {
    return false;
  }

  char *endPtr = nullptr;
  long value = strtol(token, &endPtr, 16);
  if (endPtr == token || *endPtr != '\0' || value < 0 || value > 255)
  {
    return false;
  }

  *out = (uint8_t)value;
  return true;
}

static bool parse_mac_text(const char *text, uint8_t mac[6])
{
  if (text == nullptr || mac == nullptr)
  {
    return false;
  }

  char local[24];
  memset(local, 0, sizeof(local));
  strncpy(local, text, sizeof(local) - 1);

  char *savePtr = nullptr;
  char *part = strtok_r(local, ":", &savePtr);
  int index = 0;

  while (part != nullptr && index < 6)
  {
    if (!parse_hex_byte(part, &mac[index]))
    {
      return false;
    }
    index++;
    part = strtok_r(nullptr, ":", &savePtr);
  }

  return index == 6 && part == nullptr;
}

static bool parse_serial_injected_uav(const String &payload, uav_data *out)
{
  if (out == nullptr)
  {
    return false;
  }

  char buffer[256];

  if (payload.length() >= sizeof(buffer))
  {
    HANDLE_ERROR(APP_ERR_SERIAL_PARSE);
    return false;
  }

  memset(buffer, 0, sizeof(buffer));
  payload.toCharArray(buffer, sizeof(buffer));

  // INJECT format:
  // INJECT <mac> <lat> <lon> <alt_m> <heading_deg> <speed_mps> <rssi> [base_lat] [base_lon] [uav_id] [op_id]
  char *savePtr = nullptr;
  char *token = strtok_r(buffer, " ", &savePtr);
  if (token == nullptr)
    return false;

  uav_data parsed;
  memset(&parsed, 0, sizeof(parsed));

  if (!parse_mac_text(token, parsed.mac))
  {
    return false;
  }

  token = strtok_r(nullptr, " ", &savePtr);
  if (token == nullptr)
    return false;
  parsed.lat_d = strtod(token, nullptr);

  token = strtok_r(nullptr, " ", &savePtr);
  if (token == nullptr)
    return false;
  parsed.long_d = strtod(token, nullptr);

  token = strtok_r(nullptr, " ", &savePtr);
  if (token == nullptr)
    return false;
  parsed.altitude_msl = (int)strtol(token, nullptr, 10);

  token = strtok_r(nullptr, " ", &savePtr);
  if (token == nullptr)
    return false;
  parsed.heading = (int)strtol(token, nullptr, 10);

  token = strtok_r(nullptr, " ", &savePtr);
  if (token == nullptr)
    return false;
  parsed.speed = (int)strtol(token, nullptr, 10);

  token = strtok_r(nullptr, " ", &savePtr);
  if (token == nullptr)
    return false;
  parsed.rssi = (int)strtol(token, nullptr, 10);

  token = strtok_r(nullptr, " ", &savePtr);
  if (token != nullptr)
  {
    parsed.base_lat_d = strtod(token, nullptr);
    token = strtok_r(nullptr, " ", &savePtr);
    if (token == nullptr)
    {
      return false;
    }
    parsed.base_long_d = strtod(token, nullptr);
  }

  token = strtok_r(nullptr, " ", &savePtr);
  if (token != nullptr)
  {
    strncpy(parsed.uav_id, token, ODID_ID_SIZE);
    parsed.uav_id[ODID_ID_SIZE] = '\0';

    token = strtok_r(nullptr, " ", &savePtr);
    if (token != nullptr)
    {
      strncpy(parsed.op_id, token, ODID_ID_SIZE);
      parsed.op_id[ODID_ID_SIZE] = '\0';
    }
  }
  else
  {
    strncpy(parsed.uav_id, "SERIAL", ODID_ID_SIZE);
    parsed.uav_id[ODID_ID_SIZE] = '\0';
  }

  parsed.last_seen = millis();

  *out = parsed;
  return true;
}

static bool is_valid_passkey(uint32_t passkey)
{
  return passkey >= 100000 && passkey <= 999999;
}

static void clear_ble_whitelist()
{
  while (NimBLEDevice::getWhiteListCount() > 0)
  {
    NimBLEAddress addr = NimBLEDevice::getWhiteListAddress(0);
    NimBLEDevice::whiteListRemove(addr);
  }
}

static void load_ble_passkey()
{
  blePrefs.begin(BLE_PREF_NAMESPACE, false);
  uint32_t storedPasskey = blePrefs.getULong(BLE_PREF_PASSKEY, BLE_PASSKEY_DEFAULT);
  if (!is_valid_passkey(storedPasskey))
  {
    storedPasskey = BLE_PASSKEY_DEFAULT;
    blePrefs.putULong(BLE_PREF_PASSKEY, storedPasskey);
  }

  blePasskey = storedPasskey;
}

static bool has_valid_coords(double lat, double lon)
{
  return !(lat == 0.0 && lon == 0.0) && (lat >= -90.0 && lat <= 90.0) && (lon >= -180.0 && lon <= 180.0);
}

static bool is_valid_heading(int heading)
{
  return heading >= MIN_DIR && heading <= MAX_DIR;
}

static int normalize_heading_for_output(int heading)
{
  if (is_valid_heading(heading))
  {
    return heading;
  }
  return 0;
}

static int32_t deg_to_e7(double deg)
{
  double scaled = deg * 10000000.0;
  if (scaled > 2147483647.0)
    return 2147483647;
  if (scaled < -2147483648.0)
    return -2147483648;
  return (int32_t)scaled;
}

// Get next available UAV slot or reuse existing one
static uav_data *next_uav(uint8_t *mac)
{
  for (int i = 0; i < MAX_UAVS; i++)
  {
    if (memcmp(uavs[i].mac, mac, 6) == 0)
      return &uavs[i];
  }
  for (int i = 0; i < MAX_UAVS; i++)
  {
    if (uavs[i].mac[0] == 0)
      return &uavs[i];
  }
  return &uavs[0]; // Fallback to first slot if all are used
}

static void merge_uav_event(uav_data *target, const uav_event *event)
{
  if (target == nullptr || event == nullptr)
  {
    HANDLE_ERROR(APP_ERR_NULL_POINTER);
    return;
  }

  bool wasUnused = (target->last_seen == 0);
  if (wasUnused)
  {
    memset(target, 0, sizeof(*target));
    target->heading = INV_DIR;
  }

  memcpy(target->mac, event->data.mac, sizeof(target->mac));
  target->rssi = event->data.rssi;
  target->last_seen = event->data.last_seen;

  if (event->hasBasicId)
  {
    SAFE_STRCPY(target->uav_id, event->data.uav_id);
  }

  if (event->hasLocation)
  {
    target->lat_d = event->data.lat_d;
    target->long_d = event->data.long_d;
    target->altitude_msl = event->data.altitude_msl;
    target->height_agl = event->data.height_agl;
    target->speed = event->data.speed;
    if (event->data.heading >= MIN_DIR && event->data.heading <= MAX_DIR)
    {
      target->heading = event->data.heading;
    }
  }

  if (event->hasSystem)
  {
    target->base_lat_d = event->data.base_lat_d;
    target->base_long_d = event->data.base_long_d;
  }

  if (event->hasOperatorId)
  {
    SAFE_STRCPY(target->op_id, event->data.op_id);
  }
}

static bool has_plausible_location(const uav_data *data)
{
  if (data == nullptr)
  {
    return false;
  }

  bool headingValid = (data->heading >= MIN_DIR && data->heading <= MAX_DIR);
  bool coordsValid = has_valid_coords(data->lat_d, data->long_d);
  bool altitudeValid = (data->altitude_msl != INV_ALT);
  bool speedValid = (data->speed >= 0 && data->speed != INV_SPEED_H);

  return headingValid || coordsValid || altitudeValid || (speedValid && coordsValid);
}

static void update_battery_level(unsigned long current_millis)
{
  if (!pBatteryLevelCharacteristic)
    return;
  if ((current_millis - last_battery_update) < 30000UL)
    return; // alle 30s
  last_battery_update = current_millis;

  float soc = maxlipo.cellPercent();
  if (isnan(soc) || soc < 0)
    soc = 0;
  if (soc > 100)
    soc = 100;
  uint8_t percent = (uint8_t)soc;
  pBatteryLevelCharacteristic->setValue(&percent, 1);

  float chargeRatePctPerHour = maxlipo.chargeRate();
  uint8_t chargeState = BATTERY_STATE_IDLE;
  if (chargeRatePctPerHour > BATTERY_CHARGE_STATE_DEADBAND_PCT_PER_H)
  {
    chargeState = BATTERY_STATE_CHARGING;
  }
  else if (chargeRatePctPerHour < -BATTERY_CHARGE_STATE_DEADBAND_PCT_PER_H)
  {
    chargeState = BATTERY_STATE_DISCHARGING;
  }
  if (pBatteryChargeStateCharacteristic)
  {
    pBatteryChargeStateCharacteristic->setValue(&chargeState, 1);
  }

  if (bleClientConnected)
  {
    if (pBatteryLevelCharacteristic->getSubscribedCount() == 0)
      return;

    pBatteryLevelCharacteristic->notify();
    if (pBatteryChargeStateCharacteristic)
    {
      pBatteryChargeStateCharacteristic->notify();
    }
  }
  if (percent <= 15)
  {
    signal_low_battery_blink();
  }
  float vbat = maxlipo.cellVoltage();
  Serial.printf("[BAT] SoC: %u%%, Voltage: %.3f V, ChargeRate: %.2f %%/h, State: %u\n", percent, vbat, chargeRatePctPerHour, chargeState);
};

// BLE Advertisement callback handler
class MyAdvertisedDeviceCallbacks : public NimBLEAdvertisedDeviceCallbacks
{
public:
  void onResult(NimBLEAdvertisedDevice *device) override
  {
    int len = device->getPayloadLength();
    if (len <= 0)
      return;

    uint8_t *payload = device->getPayload();
    if (len > 5 && payload[1] == 0x16 && payload[2] == 0xFA &&
        payload[3] == 0xFF && payload[4] == 0x0D)
    {
      bool hasBasicId = false;
      bool hasLocation = false;
      bool hasSystem = false;
      bool hasOperatorId = false;
      uav_data UAV;
      memset(&UAV, 0, sizeof(UAV));
      UAV.heading = INV_DIR;

      uint8_t *mac = (uint8_t *)device->getAddress().getNative();
      UAV.last_seen = millis();
      UAV.rssi = device->getRSSI();
      memcpy(UAV.mac, mac, 6);

      uint8_t *odid = &payload[6];
      switch (odid[0] & 0xF0)
      {
      case 0x00:
      {
        ODID_BasicID_data basic;
        decodeBasicIDMessage(&basic, (ODID_BasicID_encoded *)odid);
        strncpy(UAV.uav_id, (char *)basic.UASID, ODID_ID_SIZE);
        hasBasicId = true;
        break;
      }
      case 0x10:
      {
        ODID_Location_data loc;
        decodeLocationMessage(&loc, (ODID_Location_encoded *)odid);
        UAV.lat_d = loc.Latitude;
        UAV.long_d = loc.Longitude;
        UAV.altitude_msl = (int)loc.AltitudeGeo;
        UAV.height_agl = (int)loc.Height;
        UAV.speed = (int)loc.SpeedHorizontal;
        UAV.heading = (int)loc.Direction;
        hasLocation = true;
        break;
      }
      case 0x40:
      {
        ODID_System_data sys;
        decodeSystemMessage(&sys, (ODID_System_encoded *)odid);
        UAV.base_lat_d = sys.OperatorLatitude;
        UAV.base_long_d = sys.OperatorLongitude;
        hasSystem = true;
        break;
      }
      case 0x50:
      {
        ODID_OperatorID_data op;
        decodeOperatorIDMessage(&op, (ODID_OperatorID_encoded *)odid);
        strncpy(UAV.op_id, (char *)op.OperatorId, ODID_ID_SIZE);
        hasOperatorId = true;
        break;
      }
      }

      if (allow_uav_1hz(UAV.mac, UAV.last_seen))
      {
        enqueue_uav_event(&UAV, UAV_SOURCE_BLE, hasBasicId, hasLocation, hasSystem, hasOperatorId);
      }
    }
  }
};

class MyServerCallbacks : public NimBLEServerCallbacks
{
public:
  void onConnect(NimBLEServer *pServer) override
  {
    bleClientConnected = false;
    Serial.println("BLE client connecting...");
  }

  void onConnect(NimBLEServer *pServer, ble_gap_conn_desc *desc) override
  {
    bleClientConnected = false;
    if (desc == nullptr)
    {
      return;
    }
    Serial.printf("BLE client connected: %s\n", NimBLEAddress(desc->peer_id_addr).toString().c_str());

    if (desc->sec_state.encrypted && desc->sec_state.authenticated)
    {
      bleClientConnected = true;
      return;
    }

    NimBLEDevice::startSecurity(desc->conn_handle);
  }

  void onDisconnect(NimBLEServer *pServer) override
  {
    bleClientConnected = false;
    Serial.println("BLE client disconnected.");
    NimBLEDevice::startAdvertising();
  }

  uint32_t onPassKeyRequest() override
  {
    Serial.printf("BLE pairing requested, use passkey: %06lu\n", (unsigned long)blePasskey);
    return blePasskey;
  }

  bool onConfirmPIN(uint32_t pin) override
  {
    Serial.printf("BLE numeric confirmation: %06lu\n", (unsigned long)pin);
    return true;
  }

  void onAuthenticationComplete(ble_gap_conn_desc *desc) override
  {
    if (desc == nullptr)
    {
      bleClientConnected = false;
      return;
    }

    bool secure = desc->sec_state.encrypted && desc->sec_state.authenticated;
    bleClientConnected = secure;

    if (!secure)
    {
      Serial.println("BLE authentication failed, disconnecting.");
      NimBLEDevice::getServer()->disconnect(desc->conn_handle);
      return;
    }

    Serial.printf("BLE client authenticated: %s\n", NimBLEAddress(desc->peer_id_addr).toString().c_str());
    refresh_ble_whitelist_filter();
  }
};

static void refresh_ble_whitelist_filter()
{
  return; // Not used
  NimBLEAdvertising *pAdvertising = NimBLEDevice::getAdvertising();
  if (pAdvertising == nullptr)
  {
    return;
  }

  clear_ble_whitelist();

  int bondCount = NimBLEDevice::getNumBonds();
  if (bondCount <= 0)
  {
    pAdvertising->setScanFilter(false, false);
    Serial.println("BLE whitelist disabled (no bonded devices). First secure pair is allowed.");
    return;
  }

  for (int i = 0; i < bondCount; i++)
  {
    NimBLEAddress bondedAddr = NimBLEDevice::getBondedAddress(i);
    NimBLEDevice::whiteListAdd(bondedAddr);
  }

  // pAdvertising->setScanFilter(true, true);
  Serial.printf("BLE whitelist enabled for %d bonded device(s).\n", bondCount);
}

static bool set_ble_passkey(uint32_t newPasskey)
{
  if (!is_valid_passkey(newPasskey))
  {
    Serial.println("Invalid passkey. Use exactly 6 digits (100000-999999).");
    return false;
  }

  blePasskey = newPasskey;
  blePrefs.putULong(BLE_PREF_PASSKEY, blePasskey);
  NimBLEDevice::setSecurityPasskey(blePasskey);
  NimBLEDevice::deleteAllBonds();
  refresh_ble_whitelist_filter();

  NimBLEServer *pServer = NimBLEDevice::getServer();
  if (pServer != nullptr)
  {
    std::vector<uint16_t> peers = pServer->getPeerDevices();
    for (uint16_t connHandle : peers)
    {
      pServer->disconnect(connHandle);
    }
  }

  bleClientConnected = false;
  NimBLEDevice::startAdvertising();

  Serial.printf("BLE passkey updated to %06lu. Bonds cleared; re-pair required.\n", (unsigned long)blePasskey);
  return true;
}

static void handle_serial_command(const String &cmd)
{
  if (cmd.length() == 0)
  {
    return;
  }

  if (cmd.equalsIgnoreCase("HELP"))
  {
    Serial.println("Commands: HELP | PASSKEY <6digits> | SHOWPASS | SHOWCFG | SHOWQ | SHOWBLE");
    Serial.println("INJECT <mac> <lat> <lon> <alt_m> <heading_deg> <speed_mps> <rssi> [base_lat] [base_lon] [uav_id] [op_id]");
    Serial.println("SERIALINJECTJSON <ON|OFF>  (default OFF)");
    Serial.println("INJECTACK <ON|OFF>  (default OFF)");
    return;
  }

  if (cmd.equalsIgnoreCase("SHOWPASS"))
  {
    Serial.printf("Current BLE passkey: %06lu\n", (unsigned long)blePasskey);
    return;
  }

  if (cmd.equalsIgnoreCase("SHOWCFG"))
  {
    Serial.printf("serial_inject_json=%s\n", serialJsonForInjectedEnabled ? "ON" : "OFF");
    Serial.printf("inject_ack=%s\n", serialInjectAckEnabled ? "ON" : "OFF");
    return;
  }

  if (cmd.equalsIgnoreCase("SHOWQ"))
  {
    print_uav_queue_status();
    return;
  }

  if (cmd.equalsIgnoreCase("SHOWBLE"))
  {
    print_ble_notify_status();
    return;
  }

  if (cmd.startsWith("INJECTACK "))
  {
    String value = cmd.substring(10);
    value.trim();
    value.toUpperCase();

    if (value == "ON" || value == "1" || value == "TRUE")
    {
      serialInjectAckEnabled = true;
      Serial.println("inject_ack=ON");
    }
    else if (value == "OFF" || value == "0" || value == "FALSE")
    {
      serialInjectAckEnabled = false;
      Serial.println("inject_ack=OFF");
    }
    else
    {
      Serial.println("Usage: INJECTACK <ON|OFF>");
    }
    return;
  }

  if (cmd.startsWith("SERIALINJECTJSON "))
  {
    String value = cmd.substring(17);
    value.trim();
    value.toUpperCase();

    if (value == "ON" || value == "1" || value == "TRUE")
    {
      serialJsonForInjectedEnabled = true;
      Serial.println("serial_inject_json=ON");
    }
    else if (value == "OFF" || value == "0" || value == "FALSE")
    {
      serialJsonForInjectedEnabled = false;
      Serial.println("serial_inject_json=OFF");
    }
    else
    {
      Serial.println("Usage: SERIALINJECTJSON <ON|OFF>");
    }
    return;
  }

  if (cmd.startsWith("PASSKEY "))
  {
    String value = cmd.substring(8);
    value.trim();

    if (value.length() != 6)
    {
      Serial.println("Invalid PASSKEY command. Example: PASSKEY 654321");
      return;
    }

    for (size_t i = 0; i < value.length(); i++)
    {
      if (!isDigit(value[i]))
      {
        Serial.println("Passkey must contain only digits.");
        return;
      }
    }

    uint32_t newPasskey = (uint32_t)value.toInt();
    set_ble_passkey(newPasskey);
    return;
  }

  if (cmd.equalsIgnoreCase("INJECT"))
  {
    Serial.println("Usage: INJECT <mac> <lat> <lon> <alt_m> <heading_deg> <speed_mps> <rssi> [base_lat] [base_lon] [uav_id] [op_id]");
    Serial.println("Example: INJECT aa:bb:cc:dd:ee:ff 46.828929 9.437259 120 180 15 -65 46.826941 9.435680 TEST-UAV OPERATOR-1");
    return;
  }

  if (cmd.startsWith("INJECT "))
  {
    String payload = cmd.substring(7);
    payload.trim();

    uav_data injected;
    if (!parse_serial_injected_uav(payload, &injected))
    {
      Serial.println("Invalid INJECT payload.");
      Serial.println("Usage: INJECT <mac> <lat> <lon> <alt_m> <heading_deg> <speed_mps> <rssi> [base_lat] [base_lon] [uav_id] [op_id]");
      return;
    }

    bool hasSystem = has_valid_coords(injected.base_lat_d, injected.base_long_d);
    if (enqueue_uav_event(&injected, UAV_SOURCE_SERIAL, injected.uav_id[0] != '\0', true, hasSystem, injected.op_id[0] != '\0', pdMS_TO_TICKS(20)))
    {
      if (serialInjectAckEnabled)
      {
        Serial.println("Injected UAV enqueued.");
      }
    }
    else
    {
      if (serialInjectAckEnabled)
      {
        Serial.println("Failed to enqueue injected UAV (queue busy/full).");
      }
    }
    return;
  }

  Serial.println("Unknown command. Use HELP.");
}

static char serialCommandBuffer[256];
static size_t serialCommandLen = 0;

static void process_serial_commands()
{
  while (Serial.available() > 0)
  {
    char ch = (char)Serial.read();

    if (ch == '\r') continue;

    if (ch == '\n')
    {
      serialCommandBuffer[serialCommandLen] = '\0';

      String cmd(serialCommandBuffer);   // nur kurz hier
      cmd.trim();
      handle_serial_command(cmd);

      serialCommandLen = 0;
      return;
    }

    if (serialCommandLen < sizeof(serialCommandBuffer) - 1)
    {
      serialCommandBuffer[serialCommandLen++] = ch;
    }
    else
    {
      serialCommandLen = 0;
      Serial.println("[SERIAL] command too long, dropped");
      return;
    }
  }
}

// Initialize USB Serial (for JSON output) and Serial1 (for mesh/UART)
static void initializeSerial()
{
  Serial.begin(115200);
  Serial.println("USB Serial (for JSON) and UART (Serial1) initialized.");
}

// Sends JSON payload as fast as possible over USB Serial (includes basic_id).
static void send_json_fast(const uav_data *UAV, const char *extra_fields, bool emitSerial)
{
  if (UAV == nullptr)
  {
    HANDLE_ERROR(APP_ERR_NULL_POINTER);
    return;
  }

  char mac_str[18];
  snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
           UAV->mac[0], UAV->mac[1], UAV->mac[2],
           UAV->mac[3], UAV->mac[4], UAV->mac[5]);

  StaticJsonDocument<JSON_DOC_SIZE> doc;

  doc["mac"] = mac_str;
  doc["rssi"] = UAV->rssi;
  doc["drone_lat"] = UAV->lat_d;
  doc["drone_long"] = UAV->long_d;
  doc["drone_altitude"] = UAV->altitude_msl;
  doc["drone_heading"] = normalize_heading_for_output(UAV->heading);
  doc["drone_heading_valid"] = is_valid_heading(UAV->heading);
  doc["pilot_lat"] = UAV->base_lat_d;
  doc["pilot_long"] = UAV->base_long_d;
  doc["basic_id"] = UAV->uav_id;

  if (strstr(extra_fields, "BLE"))
  {
    doc["source"] = "BLE";
  }
  else if (strstr(extra_fields, "WiFi"))
  {
    doc["source"] = "WiFi";
  }
  else if (strstr(extra_fields, "Serial"))
  {
    doc["source"] = "Serial";
  }
  else
  {
    doc["source"] = "UNKNOWN";
  }

  if (measureJson(doc) >= JSON_DOC_SIZE)
  {
    HANDLE_ERROR(APP_ERR_JSON_OVERFLOW);
    return;
  }

  if (emitSerial)
  {
    serializeJson(doc, Serial);
    Serial.println();
  }

  send_waypoint_notifications(UAV, doc["source"]);
}

static char *replaceSpace(char *input) // Accept non-const pointer
{
  if (input == nullptr)
    return input;

  for (size_t i = 0; input[i] != '\0'; i++) // Iterate until null terminator
  {
    if (input[i] == ' ')
      input[i] = '_';
  }
  return input;
}

static void send_waypoint_notifications(const uav_data *UAV, const char *source)
{
  char mac_str[18];
  snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
           UAV->mac[0], UAV->mac[1], UAV->mac[2],
           UAV->mac[3], UAV->mac[4], UAV->mac[5]);

  char payload[320];

  if (has_valid_coords(UAV->lat_d, UAV->long_d))
  {
    int32_t lat_e7 = deg_to_e7(UAV->lat_d);
    int32_t lon_e7 = deg_to_e7(UAV->long_d);
    int heading_out = normalize_heading_for_output(UAV->heading);

    bool has_valid_base_coords = has_valid_coords(UAV->base_lat_d, UAV->base_long_d);
    int32_t base_lat_e7 = deg_to_e7(UAV->base_lat_d);
    int32_t base_lon_e7 = deg_to_e7(UAV->base_long_d);

    // Create local copies and replace spaces
    char uav_id_copy[ODID_ID_SIZE + 1] = {0};
    char op_id_copy[ODID_ID_SIZE + 1] = {0};

    if (UAV->uav_id[0])
    {
      strncpy(uav_id_copy, UAV->uav_id, ODID_ID_SIZE);
      replaceSpace(uav_id_copy);
    }

    if (UAV->op_id[0])
    {
      strncpy(op_id_copy, UAV->op_id, ODID_ID_SIZE);
      replaceSpace(op_id_copy);
    }

    int written = snprintf(payload, sizeof(payload),
                           "%s %s %s %ld %ld %d %d %d %d %s %ld %ld %d",
                           source, mac_str, UAV->uav_id[0] ? uav_id_copy : mac_str,
                           (long)lat_e7, (long)lon_e7, UAV->altitude_msl, heading_out, UAV->speed, UAV->rssi,
                           UAV->op_id[0] ? op_id_copy : mac_str, (long)base_lat_e7, (long)base_lon_e7, has_valid_base_coords ? 1 : 0);

    if (written < 0 || written >= sizeof(payload))
    {
      HANDLE_ERROR(APP_ERR_UNKNOWN);
      return;
    }

    send_ble_notification(payload);
  }
}

static void send_ble_notification(const char *message)
{
  bleNotifyAttemptCount++;

  if (message == nullptr)
  {
    bleNotifyErrorCount++;
    HANDLE_ERROR(APP_ERR_NULL_POINTER);
    return;
  }

  if (!bleClientConnected ||
      pTelemetryCharacteristic == nullptr ||
      bleTxMutex == nullptr)
  {
    bleNotifyNoClientCount++;
    return;
  }

  if (xSemaphoreTake(bleTxMutex, pdMS_TO_TICKS(10)) != pdTRUE)
  {
    bleNotifyMutexTimeoutCount++;
    HANDLE_ERROR(APP_ERR_MUTEX_TIMEOUT);
    return;
  }

  std::string framed(message);
  framed.push_back('\0');

  const size_t maxChunkSize = 244;
  size_t messageLength = framed.size();
  size_t offset = 0;

  while (offset < messageLength)
  {
    size_t chunkLength = messageLength - offset;

    if (chunkLength > maxChunkSize)
    {
      chunkLength = maxChunkSize;
    }

    std::string chunk(framed.data() + offset, chunkLength);

    if (!bleClientConnected)
    {
      bleNotifyErrorCount++;
      HANDLE_ERROR(APP_ERR_BLE_NOTIFY);
      break;
    }

    if (pTelemetryCharacteristic->getSubscribedCount() == 0)
    {
      bleNotifyNoSubCount++;
      xSemaphoreGive(bleTxMutex);
      return;
    }

    pTelemetryCharacteristic->setValue(chunk);

    if (!bleClientConnected)
    {
      bleNotifyErrorCount++;
      break;
    }

    pTelemetryCharacteristic->notify();
    if (bleClientConnected)
    {
      bleNotifySentCount++;
    }
    else
    {
      bleNotifyChunkFailCount++;
      break;
    }

    offset += chunkLength;
    vTaskDelay(pdMS_TO_TICKS(5));
    esp_task_wdt_reset();
  }

  xSemaphoreGive(bleTxMutex);
}

// Wi-Fi promiscuous packet callback
static void callback(void *buffer, wifi_promiscuous_pkt_type_t type)
{
  if (type != WIFI_PKT_MGMT)
    return;

  if (buffer == nullptr)
  {
    HANDLE_ERROR(APP_ERR_NULL_POINTER);
    return;
  }

  wifi_promiscuous_pkt_t *packet = (wifi_promiscuous_pkt_t *)buffer;
  uint8_t *payload = packet->payload;
  int length = packet->rx_ctrl.sig_len;

  if (length <= 0)
  {
    return;
  }

  if (length < 16)
  {
    return;
  }

  static const uint8_t nan_dest[6] = {0x51, 0x6f, 0x9a, 0x01, 0x00, 0x00};
  if (memcmp(nan_dest, &payload[4], 6) == 0)
  {
    char nanSourceMac[6] = {0};
    if (odid_wifi_receive_message_pack_nan_action_frame(&UAS_data, nanSourceMac, payload, length) == 0)
    {
      uav_data UAV;
      memset(&UAV, 0, sizeof(UAV));
      UAV.heading = INV_DIR;
      memcpy(UAV.mac, &payload[10], 6);
      UAV.rssi = packet->rx_ctrl.rssi;
      UAV.last_seen = millis();

      bool hasBasicId = false;
      bool hasLocation = false;
      bool hasSystem = false;
      bool hasOperatorId = false;

      if (UAS_data.BasicIDValid[0])
      {
        strncpy(UAV.uav_id, (char *)UAS_data.BasicID[0].UASID, ODID_ID_SIZE);
        hasBasicId = true;
      }
      if (UAS_data.LocationValid)
      {
        UAV.lat_d = UAS_data.Location.Latitude;
        UAV.long_d = UAS_data.Location.Longitude;
        UAV.altitude_msl = (int)UAS_data.Location.AltitudeGeo;
        UAV.height_agl = (int)UAS_data.Location.Height;
        UAV.speed = (int)UAS_data.Location.SpeedHorizontal;
        UAV.heading = (int)UAS_data.Location.Direction;
        hasLocation = has_plausible_location(&UAV);
      }
      if (UAS_data.SystemValid)
      {
        UAV.base_lat_d = UAS_data.System.OperatorLatitude;
        UAV.base_long_d = UAS_data.System.OperatorLongitude;
        hasSystem = true;
      }
      if (UAS_data.OperatorIDValid)
      {
        strncpy(UAV.op_id, (char *)UAS_data.OperatorID.OperatorId, ODID_ID_SIZE);
        hasOperatorId = true;
      }

      if (allow_uav_1hz(UAV.mac, UAV.last_seen))
      {
        enqueue_uav_event(&UAV, UAV_SOURCE_WIFI, hasBasicId, hasLocation, hasSystem, hasOperatorId);
      }
    }
    else
    {
      return;
    }
  }
  else if (payload[0] == 0x80)
  {
    int offset = 36;
    while ((offset + 1) < length)
    {
      int typ = payload[offset];
      int len = payload[offset + 1];

      if ((offset + 2 + len) > length)
      {
        break;
      }

      if (typ == 0xdd)
      {
        if (len < 5)
        {
          offset += len + 2;
          continue;
        }

        const uint8_t *vendor = &payload[offset + 2];
        bool isAstmRidVendorIe =
            (vendor[0] == 0xfa && vendor[1] == 0x0b && vendor[2] == 0xbc && vendor[3] == 0x0d);
        if (!isAstmRidVendorIe)
        {
          offset += len + 2;
          continue;
        }

        int j = offset + 7; // 2-byte IE header + 3-byte OUI + 1-byte OUI type + 1-byte message counter
        int ieEnd = offset + 2 + len;
        int packLen = ieEnd - j;
        if (packLen <= 0 || j >= length)
        {
          break;
        }

        memset(&UAS_data, 0, sizeof(UAS_data));
        int decodeLen = odid_message_process_pack(&UAS_data, &payload[j], (size_t)packLen);
        if (decodeLen != packLen)
        {
          break;
        }

        uav_data UAV;
        memset(&UAV, 0, sizeof(UAV));
        UAV.heading = INV_DIR;
        memcpy(UAV.mac, &payload[10], 6);
        UAV.rssi = packet->rx_ctrl.rssi;
        UAV.last_seen = millis();

        bool hasBasicId = false;
        bool hasLocation = false;
        bool hasSystem = false;
        bool hasOperatorId = false;

        if (UAS_data.BasicIDValid[0])
        {
          strncpy(UAV.uav_id, (char *)UAS_data.BasicID[0].UASID, ODID_ID_SIZE);
          hasBasicId = true;
        }
        if (UAS_data.LocationValid)
        {
          UAV.lat_d = UAS_data.Location.Latitude;
          UAV.long_d = UAS_data.Location.Longitude;
          UAV.altitude_msl = (int)UAS_data.Location.AltitudeGeo;
          UAV.height_agl = (int)UAS_data.Location.Height;
          UAV.speed = (int)UAS_data.Location.SpeedHorizontal;
          UAV.heading = (int)UAS_data.Location.Direction;
          hasLocation = has_plausible_location(&UAV);
        }
        if (UAS_data.SystemValid)
        {
          UAV.base_lat_d = UAS_data.System.OperatorLatitude;
          UAV.base_long_d = UAS_data.System.OperatorLongitude;
          hasSystem = true;
        }
        if (UAS_data.OperatorIDValid)
        {
          strncpy(UAV.op_id, (char *)UAS_data.OperatorID.OperatorId, ODID_ID_SIZE);
          hasOperatorId = true;
        }

        bool hasAnyRidData = hasBasicId || hasLocation || hasSystem || hasOperatorId;
        if (!hasAnyRidData)
        {
          break;
        }

        if (allow_uav_1hz(UAV.mac, UAV.last_seen))
        {
          enqueue_uav_event(&UAV, UAV_SOURCE_WIFI, hasBasicId, hasLocation, hasSystem, hasOperatorId);
        }

        break;
      }
      offset += len + 2;
    }
  }
}

// BLE scanning task running on core 0
static void bleScanTask(void *parameter)
{
  esp_task_wdt_add(NULL);

  pBLEScan->start(0, nullptr, false);

  while (true)
  {
    esp_task_wdt_reset();
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

// UAV processing task: single writer for the UAV DB + single sender
static void uavProcessTask(void *parameter)
{
  esp_task_wdt_add(NULL);

  for (;;)
  {
    esp_task_wdt_reset();

    static unsigned long last_heap_log = 0;

    uav_event event;
    if (xQueueReceive(uavEventQueue, &event, pdMS_TO_TICKS(100)) == pdTRUE)
    {
      uavQueueDequeueCount++;
      update_uav_queue_metrics();
      last_drone_seen = millis();
      signal_data_received();
      uav_data *dbUAV = next_uav(event.data.mac);
      merge_uav_event(dbUAV, &event);

      bool emitSerial = true;
      if (event.source == UAV_SOURCE_SERIAL && !serialJsonForInjectedEnabled)
      {
        emitSerial = false;
      }

      send_json_fast(dbUAV, source_extra_fields(event.source), emitSerial);
    }

    unsigned long current_millis = millis();

    if ((current_millis - last_heap_log) > 60000UL)
    {
      print_heap_status();
      print_uav_queue_status();
      print_ble_notify_status();
      last_heap_log = current_millis;
    }

    if ((current_millis - last_status) > 60000UL)
    {
      Serial.println("{\"heartbeat\":\"Device is active and running.\"}");
      signal_idle_blink();
      last_status = current_millis;
    }

    update_status_pixel(current_millis);
    update_battery_level(current_millis);

    // BLE heartbeat: notify connected client every 10s when no drone has been seen for 15s
    if (bleClientConnected &&
        (last_drone_seen == 0 || (current_millis - last_drone_seen) > 15000UL) &&
        (current_millis - last_ble_heartbeat) > 10000UL)
    {
      send_ble_notification("{\"type\":\"heartbeat\",\"status\":\"no_drone_seen\"}");
      last_ble_heartbeat = current_millis;
    }
  }
}

static void buttonTask(void *parameter)
{
  for (;;)
  {
    checkButtonForDeepSleep();
    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

void setup()
{

  initializeSerial();
  delay(300);

  Serial.printf("Reset reason: %d\n", esp_reset_reason());  

  esp_sleep_wakeup_cause_t wakeCause = esp_sleep_get_wakeup_cause();
  if (wakeCause == ESP_SLEEP_WAKEUP_EXT0)
  {
    // After EXT0 wake, return RTC IO pin to normal GPIO mode.
    rtc_gpio_deinit((gpio_num_t)BUTTON_PIN);
  }

  pinMode(BUTTON_PIN, INPUT_PULLUP);
  buttonRawState = digitalRead(BUTTON_PIN);
  buttonDebouncedState = buttonRawState;
  buttonLastEdgeAt = millis();
  pinMode(LED_BUILTIN, OUTPUT);
  digitalWrite(LED_BUILTIN, LOW);
  pinMode(38, OUTPUT);
  digitalWrite(38, HIGH);
  setCpuFrequencyMhz(160);
  nvs_flash_init();
  last_status = millis();
  if (wakeCause == ESP_SLEEP_WAKEUP_EXT0)
  {
    requireButtonReleaseAfterWake = true;
    Serial.println("Wakeup by button (EXT0). Release button to re-arm sleep.");
  }
  load_ble_passkey();
  uavEventQueue = xQueueCreate(UAV_EVENT_QUEUE_LENGTH, sizeof(uav_event));
  if (uavEventQueue == nullptr)
  {
    Serial.println("Failed to create UAV event queue.");
  }

  // Initialize Wi-Fi
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();

  esp_wifi_set_promiscuous(true);
  esp_wifi_set_promiscuous_rx_cb(&callback);
  esp_wifi_set_channel(6, WIFI_SECOND_CHAN_NONE);

  uint64_t chipId = ESP.getEfuseMac();           // 48-bit factory unique base MAC
  uint16_t suffix = (uint16_t)(chipId & 0xFFFF); // last 4 hex chars
  char bleName[20];
  snprintf(bleName, sizeof(bleName), "DroneID-%04X", suffix);

  // Initialize BLE scanning
  BLEDevice::init(bleName);
  NimBLEDevice::setMTU(247);
  NimBLEDevice::setSecurityAuth(true, true, true);
  NimBLEDevice::setSecurityIOCap(ESP_IO_CAP_OUT);
  NimBLEDevice::setSecurityPasskey(blePasskey);
  NimBLEDevice::setSecurityInitKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);
  NimBLEDevice::setSecurityRespKey(ESP_BLE_ENC_KEY_MASK | ESP_BLE_ID_KEY_MASK);

  NimBLEServer *pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new MyServerCallbacks());

  NimBLEService *pService = pServer->createService(BLE_SERVICE_UUID);
  pTelemetryCharacteristic = pService->createCharacteristic(
      BLE_TELEMETRY_UUID,
      NIMBLE_PROPERTY::READ_ENC | NIMBLE_PROPERTY::NOTIFY);
  pTelemetryCharacteristic->setValue("ready");
  pService->start();

  NimBLEService *pBattService = pServer->createService("0000180f-0000-1000-8000-00805f9b34fb");
  pBatteryLevelCharacteristic = pBattService->createCharacteristic("00002a19-0000-1000-8000-00805f9b34fb", NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  pBatteryChargeStateCharacteristic = pBattService->createCharacteristic(BLE_BATTERY_CHARGE_STATE_UUID, NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY);
  pBatteryLevelCharacteristic->setValue((uint8_t)100);
  pBatteryChargeStateCharacteristic->setValue((uint8_t)BATTERY_STATE_IDLE);
  pBattService->start();

  NimBLEAdvertising *pAdvertising = NimBLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(BLE_SERVICE_UUID);
  pAdvertising->addServiceUUID("0000180f-0000-1000-8000-00805f9b34fb");
  refresh_ble_whitelist_filter();
  pAdvertising->start();

  bleTxMutex = xSemaphoreCreateMutex();

  pBLEScan = BLEDevice::getScan();
  pBLEScan->setAdvertisedDeviceCallbacks(new MyAdvertisedDeviceCallbacks());
  pBLEScan->setActiveScan(true);
  pBLEScan->setInterval(160);
  pBLEScan->setWindow(80);

  // Initialize UAV tracking array
  memset(uavs, 0, sizeof(uavs));

  // Battery monitor init
  Wire.begin();
  if (!maxlipo.begin())
  {
    Serial.println("MAX17048 not found! Check wiring.");
  }
  else
  {
    Serial.println("MAX17048 battery monitor initialized.");
  }

  statusPixel.begin();
  statusPixel.setBrightness(100);
  signal_idle_blink();
  delay(250);
  signal_data_received();
  delay(250);
  signal_idle_blink();
  delay(250);
  signal_data_received();
  delay(250);

  esp_task_wdt_init(WATCHDOG_TIMEOUT_SEC, true);

  // Create tasks for BLE scanning and single UAV event processing
  xTaskCreatePinnedToCore(bleScanTask, "BLEScanTask", 16000, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(uavProcessTask, "UAVProcessTask", 20000, NULL, 1, NULL, 1);
  xTaskCreatePinnedToCore(buttonTask, "ButtonTask", 4096, NULL, 1, NULL, 1);
}

// --- Deep Sleep Button Logic ---

static void checkButtonForDeepSleep()
{
  int rawState = digitalRead(BUTTON_PIN);
  if (rawState != buttonRawState)
  {
    buttonRawState = rawState;
    buttonLastEdgeAt = millis();
  }

  if ((millis() - buttonLastEdgeAt) >= BUTTON_DEBOUNCE_MS)
  {
    buttonDebouncedState = buttonRawState;
  }

  int buttonState = buttonDebouncedState;

  if (requireButtonReleaseAfterWake)
  {
    if (buttonState == HIGH)
    {
      requireButtonReleaseAfterWake = false;
      buttonWasPressed = false;
      buttonPressStart = 0;
    }
    return;
  }

  if (buttonState == LOW)
  { // Button pressed (assuming pull-up)
    set_status_pixel(255, 0, 0);

    if (!buttonWasPressed)
    {
      buttonPressStart = millis();
      buttonWasPressed = true;
      buttonSleepArmed = false;
    }
    else if (!buttonSleepArmed && (millis() - buttonPressStart >= BUTTON_HOLD_TIME_MS))
    {
      buttonSleepArmed = true;
      Serial.println("Button held for 5 seconds, release to enter deep sleep...");
    }
  }
  else
  {
    if (buttonWasPressed && buttonSleepArmed)
    {
      Serial.println("Entering deep sleep...");
      signal_data_received();
      delay(250);
      clear_status_pixel();

      // Configure wakeup: wake on next button press (LOW)
      rtc_gpio_pullup_en((gpio_num_t)BUTTON_PIN);
      rtc_gpio_pulldown_dis((gpio_num_t)BUTTON_PIN);
      esp_sleep_enable_ext0_wakeup((gpio_num_t)BUTTON_PIN, 0);
      Serial.flush();
      delay(100);
      esp_deep_sleep_start();
    }

    clear_status_pixel();
    buttonWasPressed = false;
    buttonSleepArmed = false;
  }
}

void loop()
{
  static uint32_t last = 0;

  process_serial_commands();
  delay(1);
}