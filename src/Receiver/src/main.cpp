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

#define MAX_UAVS 20

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
};

uav_data uavs[MAX_UAVS] = {};

BLEScan *pBLEScan = nullptr;
NimBLECharacteristic *pTelemetryCharacteristic = nullptr;
NimBLECharacteristic *pBatteryLevelCharacteristic = nullptr;
NimBLECharacteristic *pBatteryChargeStateCharacteristic = nullptr;
SemaphoreHandle_t bleTxMutex = nullptr;
QueueHandle_t uavEventQueue = nullptr;
volatile bool bleClientConnected = false;
ODID_UAS_Data UAS_data;
unsigned long last_status = 0;
unsigned long last_drone_seen = 0;
unsigned long last_ble_heartbeat = 0;
unsigned long last_pixel_idle_blink = 0;
unsigned long pixel_off_at = 0;
unsigned long data_signal_until = 0;
Adafruit_MAX17048 maxlipo;
unsigned long last_battery_update = 0;
Preferences blePrefs;
uint32_t blePasskey = 123456;
String serialCommandBuffer;
unsigned long buttonPressStart = 0;
unsigned long buttonLastEdgeAt = 0;
int buttonRawState = HIGH;
int buttonDebouncedState = HIGH;
bool buttonWasPressed = false;
bool requireButtonReleaseAfterWake = false;
bool buttonSleepArmed = false;

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
void callback(void *, wifi_promiscuous_pkt_type_t);
void send_json_fast(const uav_data *UAV, const char *extra_fields = "");
void send_ble_notification(const char *message);
void send_waypoint_notifications(const uav_data *UAV, const char *source);
void refresh_ble_whitelist_filter();
void process_serial_commands();
void handle_serial_command(const String &cmd);
bool set_ble_passkey(uint32_t newPasskey);
bool enqueue_uav_event(const uav_data *data, uav_source_t source);
const char *source_extra_fields(uav_source_t source);
bool parse_serial_injected_uav(const String &payload, uav_data *out);

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
  set_status_pixel(0, 0, 255, 250);
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

bool enqueue_uav_event(const uav_data *data, uav_source_t source)
{
  if (uavEventQueue == nullptr || data == nullptr)
  {
    return false;
  }

  uav_event event;
  memset(&event, 0, sizeof(event));
  memcpy(&event.data, data, sizeof(uav_data));
  event.source = source;

  return xQueueSend(uavEventQueue, &event, 0) == pdTRUE;
}

const char *source_extra_fields(uav_source_t source)
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

bool parse_serial_injected_uav(const String &payload, uav_data *out)
{
  if (out == nullptr)
  {
    return false;
  }

  char buffer[256];
  memset(buffer, 0, sizeof(buffer));
  payload.toCharArray(buffer, sizeof(buffer));

  // INJECT format:
  // INJECT <mac> <lat> <lon> <alt_m> <heading_deg> <speed_mps> [base_lat] [base_lon] [uav_id]
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

  token = strtok_r(nullptr, "", &savePtr);
  if (token != nullptr)
  {
    while (*token == ' ')
    {
      token++;
    }
    strncpy(parsed.uav_id, token, ODID_ID_SIZE);
  }
  else
  {
    strncpy(parsed.uav_id, "SERIAL", ODID_ID_SIZE);
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
uav_data *next_uav(uint8_t *mac)
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

static void update_battery_level(unsigned long current_millis)
{
  if (!pBatteryLevelCharacteristic)
    return;
  if ((current_millis - last_battery_update) < 10000UL)
    return; // alle 10s
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
      uav_data UAV;
      memset(&UAV, 0, sizeof(UAV));

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
        break;
      }
      case 0x40:
      {
        ODID_System_data sys;
        decodeSystemMessage(&sys, (ODID_System_encoded *)odid);
        UAV.base_lat_d = sys.OperatorLatitude;
        UAV.base_long_d = sys.OperatorLongitude;
        break;
      }
      case 0x50:
      {
        ODID_OperatorID_data op;
        decodeOperatorIDMessage(&op, (ODID_OperatorID_encoded *)odid);
        strncpy(UAV.op_id, (char *)op.OperatorId, ODID_ID_SIZE);
        break;
      }
      }

      enqueue_uav_event(&UAV, UAV_SOURCE_BLE);
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

void refresh_ble_whitelist_filter()
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

bool set_ble_passkey(uint32_t newPasskey)
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

void handle_serial_command(const String &cmd)
{
  if (cmd.length() == 0)
  {
    return;
  }

  if (cmd.equalsIgnoreCase("HELP"))
  {
    Serial.println("Commands: HELP | PASSKEY <6digits> | SHOWPASS");
    Serial.println("INJECT <mac> <lat> <lon> <alt_m> <heading_deg> <speed_mps> [base_lat] [base_lon] [uav_id]");
    return;
  }

  if (cmd.equalsIgnoreCase("SHOWPASS"))
  {
    Serial.printf("Current BLE passkey: %06lu\n", (unsigned long)blePasskey);
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
    Serial.println("Usage: INJECT <mac> <lat> <lon> <alt_m> <heading_deg> <speed_mps> [base_lat] [base_lon] [uav_id]");
    Serial.println("Example: INJECT aa:bb:cc:dd:ee:ff 52.5208 13.4095 120 180 15 52.5200 13.4050 TEST-UAV");
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
      Serial.println("Usage: INJECT <mac> <lat> <lon> <alt_m> <heading_deg> <speed_mps> [base_lat] [base_lon] [uav_id]");
      return;
    }

    if (enqueue_uav_event(&injected, UAV_SOURCE_SERIAL))
    {
      Serial.println("Injected UAV enqueued.");
    }
    else
    {
      Serial.println("Failed to enqueue injected UAV (queue busy/full).");
    }
    return;
  }

  Serial.println("Unknown command. Use HELP.");
}

void process_serial_commands()
{
  while (Serial.available() > 0)
  {
    char ch = (char)Serial.read();
    if (ch == '\r')
    {
      continue;
    }

    if (ch == '\n')
    {
      serialCommandBuffer.trim();
      handle_serial_command(serialCommandBuffer);
      serialCommandBuffer = "";
      continue;
    }

    if (serialCommandBuffer.length() < 240)
    {
      serialCommandBuffer += ch;
    }
  }
}

// Initialize USB Serial (for JSON output) and Serial1 (for mesh/UART)
void initializeSerial()
{
  Serial.begin(115200);
  Serial.println("USB Serial (for JSON) and UART (Serial1) initialized.");
}

// Sends JSON payload as fast as possible over USB Serial (includes basic_id).
void send_json_fast(const uav_data *UAV, const char *extra_fields)
{
  char mac_str[18];
  snprintf(mac_str, sizeof(mac_str), "%02x:%02x:%02x:%02x:%02x:%02x",
           UAV->mac[0], UAV->mac[1], UAV->mac[2],
           UAV->mac[3], UAV->mac[4], UAV->mac[5]);
  char json_msg[256];
  snprintf(json_msg, sizeof(json_msg),
           "{\"mac\":\"%s\",\"rssi\":%d,\"drone_lat\":%.6f,\"drone_long\":%.6f,"
           "\"drone_altitude\":%d,\"drone_heading\":%d,\"pilot_lat\":%.6f,\"pilot_long\":%.6f,"
           "\"basic_id\":\"%s\"%s}",
           mac_str, UAV->rssi, UAV->lat_d, UAV->long_d, UAV->altitude_msl, UAV->heading,
           UAV->base_lat_d, UAV->base_long_d, UAV->uav_id, extra_fields);
  Serial.println(json_msg);

  const char *source = "UNKNOWN";
  if (strstr(extra_fields, "\"source\":\"BLE\""))
  {
    source = "BLE";
  }
  else if (strstr(extra_fields, "\"source\":\"WiFi\""))
  {
    source = "WiFi";
  }
  send_waypoint_notifications(UAV, source);
}

void send_waypoint_notifications(const uav_data *UAV, const char *source)
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

    bool has_valid_base_coords = has_valid_coords(UAV->base_lat_d, UAV->base_long_d);
    int32_t base_lat_e7 = deg_to_e7(UAV->base_lat_d);
    int32_t base_lon_e7 = deg_to_e7(UAV->base_long_d);

    snprintf(payload, sizeof(payload),
             "{\"type\":\"waypoint\",\"role\":\"drone\",\"source\":\"%s\",\"mac\":\"%s\",\"waypoint\":{\"id\":\"drone_%02x%02x%02x%02x%02x%02x\",\"Drone\":\"%s\",\"latitudeI\":%ld,\"longitudeI\":%ld,\"altitude\":%d,\"heading\":%d,\"speed\":%d,\"base_latitudeI\":%ld,\"base_longitudeI\":%ld, \"base_valid\":%d}}",
             source, mac_str, UAV->mac[0], UAV->mac[1], UAV->mac[2], UAV->mac[3], UAV->mac[4], UAV->mac[5],
             UAV->uav_id[0] ? UAV->uav_id : mac_str,
             (long)lat_e7, (long)lon_e7, UAV->altitude_msl, UAV->heading, UAV->speed, (long)base_lat_e7, (long)base_lon_e7, has_valid_base_coords ? 1 : 0);
    send_ble_notification(payload);
  }
}

void send_ble_notification(const char *message)
{
  if (!bleClientConnected || pTelemetryCharacteristic == nullptr || bleTxMutex == nullptr)
  {
    return;
  }

  if (xSemaphoreTake(bleTxMutex, pdMS_TO_TICKS(10)) != pdTRUE)
  {
    return;
  }

  std::string framed(message);
  framed.push_back('\0');

  const size_t maxChunkSize = 244; // BLE payload limit with some overhead
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
    pTelemetryCharacteristic->setValue(chunk);
    pTelemetryCharacteristic->notify();
    offset += chunkLength;
    delay(5);
  }

  xSemaphoreGive(bleTxMutex);
}

// Wi-Fi promiscuous packet callback
void callback(void *buffer, wifi_promiscuous_pkt_type_t type)
{
  if (type != WIFI_PKT_MGMT)
    return;

  wifi_promiscuous_pkt_t *packet = (wifi_promiscuous_pkt_t *)buffer;
  uint8_t *payload = packet->payload;
  int length = packet->rx_ctrl.sig_len;

  static const uint8_t nan_dest[6] = {0x51, 0x6f, 0x9a, 0x01, 0x00, 0x00};
  if (memcmp(nan_dest, &payload[4], 6) == 0)
  {
    if (odid_wifi_receive_message_pack_nan_action_frame(&UAS_data, nullptr, payload, length) == 0)
    {
      uav_data UAV;
      memset(&UAV, 0, sizeof(UAV));
      memcpy(UAV.mac, &payload[10], 6);
      UAV.rssi = packet->rx_ctrl.rssi;
      UAV.last_seen = millis();

      if (UAS_data.BasicIDValid[0])
      {
        strncpy(UAV.uav_id, (char *)UAS_data.BasicID[0].UASID, ODID_ID_SIZE);
      }
      if (UAS_data.LocationValid)
      {
        UAV.lat_d = UAS_data.Location.Latitude;
        UAV.long_d = UAS_data.Location.Longitude;
        UAV.altitude_msl = (int)UAS_data.Location.AltitudeGeo;
        UAV.height_agl = (int)UAS_data.Location.Height;
        UAV.speed = (int)UAS_data.Location.SpeedHorizontal;
        UAV.heading = (int)UAS_data.Location.Direction;
      }
      if (UAS_data.SystemValid)
      {
        UAV.base_lat_d = UAS_data.System.OperatorLatitude;
        UAV.base_long_d = UAS_data.System.OperatorLongitude;
      }
      if (UAS_data.OperatorIDValid)
      {
        strncpy(UAV.op_id, (char *)UAS_data.OperatorID.OperatorId, ODID_ID_SIZE);
      }

      enqueue_uav_event(&UAV, UAV_SOURCE_WIFI);
    }
  }
  else if (payload[0] == 0x80)
  {
    int offset = 36;
    while (offset < length)
    {
      int typ = payload[offset];
      int len = payload[offset + 1];
      if ((typ == 0xdd) &&
          (((payload[offset + 2] == 0x90 && payload[offset + 3] == 0x3a && payload[offset + 4] == 0xe6)) ||
           ((payload[offset + 2] == 0xfa && payload[offset + 3] == 0x0b && payload[offset + 4] == 0xbc))))
      {
        int j = offset + 7;
        if (j < length)
        {
          memset(&UAS_data, 0, sizeof(UAS_data));
          odid_message_process_pack(&UAS_data, &payload[j], length - j);

          uav_data UAV;
          memset(&UAV, 0, sizeof(UAV));
          memcpy(UAV.mac, &payload[10], 6);
          UAV.rssi = packet->rx_ctrl.rssi;
          UAV.last_seen = millis();

          if (UAS_data.BasicIDValid[0])
          {
            strncpy(UAV.uav_id, (char *)UAS_data.BasicID[0].UASID, ODID_ID_SIZE);
          }
          if (UAS_data.LocationValid)
          {
            UAV.lat_d = UAS_data.Location.Latitude;
            UAV.long_d = UAS_data.Location.Longitude;
            UAV.altitude_msl = (int)UAS_data.Location.AltitudeGeo;
            UAV.height_agl = (int)UAS_data.Location.Height;
            UAV.speed = (int)UAS_data.Location.SpeedHorizontal;
            UAV.heading = (int)UAS_data.Location.Direction;
          }
          if (UAS_data.SystemValid)
          {
            UAV.base_lat_d = UAS_data.System.OperatorLatitude;
            UAV.base_long_d = UAS_data.System.OperatorLongitude;
          }
          if (UAS_data.OperatorIDValid)
          {
            strncpy(UAV.op_id, (char *)UAS_data.OperatorID.OperatorId, ODID_ID_SIZE);
          }

          enqueue_uav_event(&UAV, UAV_SOURCE_WIFI);
        }
      }
      offset += len + 2;
    }
  }
}

// BLE scanning task running on core 0
void bleScanTask(void *parameter)
{
  for (;;)
  {
    pBLEScan->start(1, false);
    pBLEScan->clearResults();
    delay(20);
  }
}

// UAV processing task: single writer for the UAV DB + single sender
void uavProcessTask(void *parameter)
{
  for (;;)
  {
    uav_event event;
    if (xQueueReceive(uavEventQueue, &event, pdMS_TO_TICKS(100)) == pdTRUE)
    {
      last_drone_seen = millis();
      signal_data_received();
      uav_data *dbUAV = next_uav(event.data.mac);
      memcpy(dbUAV, &event.data, sizeof(uav_data));
      send_json_fast(dbUAV, source_extra_fields(event.source));
    }

    unsigned long current_millis = millis();

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

void setup()
{
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
  initializeSerial();
  last_status = millis();
  if (wakeCause == ESP_SLEEP_WAKEUP_EXT0)
  {
    requireButtonReleaseAfterWake = true;
    Serial.println("Wakeup by button (EXT0). Release button to re-arm sleep.");
  }
  load_ble_passkey();
  uavEventQueue = xQueueCreate(64, sizeof(uav_event));
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
  pBLEScan->setInterval(100);
  pBLEScan->setWindow(99);

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

  // Create tasks for BLE scanning and single UAV event processing
  xTaskCreatePinnedToCore(bleScanTask, "BLEScanTask", 10000, NULL, 1, NULL, 0);
  xTaskCreatePinnedToCore(uavProcessTask, "UAVProcessTask", 12000, NULL, 1, NULL, 1);
}

// --- Deep Sleep Button Logic ---

void checkButtonForDeepSleep()
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
  process_serial_commands();
  checkButtonForDeepSleep();
}