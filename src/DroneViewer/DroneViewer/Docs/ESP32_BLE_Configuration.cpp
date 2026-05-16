/* 
 * ESP32 BLE Empfehlungen für DroneViewer
 * ========================================
 * 
 * Problem: Standard BLE MTU ist 20-23 Bytes
 * Lösung: Daten in Chunks senden ODER MTU erhöhen
 * 
 * Option 1: MTU auf ESP32 erhöhen (empfohlen)
 * -------------------------------------------
 */

// In deinem ESP32-Code nach pServer->createService():
NimBLEDevice::setMTU(512);  // Erhöhe MTU auf 512 bytes

// Oder per Connection:
// pServer->updateConnParams(conn_id, 0, 0, 0, 400);


/* 
 * Option 2: Daten in Chunks senden
 * ---------------------------------
 * Wenn MTU-Erhöhung nicht funktioniert, sende JSON in 20-Byte-Chunks
 */

void sendLargeJSON(NimBLECharacteristic* pChar, const char* json) {
    size_t len = strlen(json);
    size_t chunkSize = 20;  // Standard MTU - 3 bytes overhead

    for (size_t i = 0; i < len; i += chunkSize) {
        size_t remaining = len - i;
        size_t currentChunk = (remaining < chunkSize) ? remaining : chunkSize;

        pChar->setValue((uint8_t*)(json + i), currentChunk);
        pChar->notify();

        delay(10);  // Kleine Pause zwischen Chunks
    }
}

// Verwendung:
// String json = "{\"type\":\"heartbeat\",\"status\":\"no_drone_seen\"}";
// sendLargeJSON(pTelemetryCharacteristic, json.c_str());


/* 
 * Option 3: Notification statt setValue + notify
 * -----------------------------------------------
 */

// Statt:
// pTelemetryCharacteristic->setValue("ready");
// pTelemetryCharacteristic->notify();

// Besser:
pTelemetryCharacteristic->notify((uint8_t*)jsonData.c_str(), jsonData.length());


/*
 * Debugging: MTU auf ESP32 prüfen
 * --------------------------------
 */

class ServerCallbacks: public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* pServer, ble_gap_conn_desc* desc) {
        uint16_t mtu = pServer->getPeerMTU(desc->conn_handle);
        Serial.printf("Client connected, MTU: %d\n", mtu);

        // Optional: MTU-Exchange anfordern
        pServer->updateConnParams(desc->conn_handle, 24, 48, 0, 60);
    }
};

/*
 * DroneViewer erwartet:
 * ---------------------
 * - Multi-Chunk-Empfang funktioniert automatisch
 * - JSON wird gepuffert bis vollständig
 * - 2-Sekunden-Timeout bei unvollständigen Daten
 * - MTU von 512 bytes wird angefordert (falls ESP32 unterstützt)
 */
