// ============================================================
//  TARGET FIRMWARE v4.0 — ESP32 DevKit
//  Role: Execution Plane
//  - Receives validated firmware via UART0 from Gateway
//  - Writes directly to OTA partitions via <Update.h>
//  - Manages hardware-triggered boot modes and rollbacks
// ============================================================

#include <Arduino.h>
#include <Update.h>
#include "esp_ota_ops.h"
#include "esp_partition.h"

// ============================================================
//  CONFIGURATION
// ============================================================
#define UART_BAUD   115200
#define CHUNK_SIZE  200

// ============================================================
//  SYSTEM STATE
// ============================================================
enum TargetState {
  STATE_COMMAND,
  STATE_RECEIVING_OTA
};

TargetState currentState = STATE_COMMAND;

uint32_t otaTotalSize  = 0;
uint32_t otaRemaining  = 0;
uint32_t chunkExpected = 0;
uint32_t chunkReceived = 0;
uint8_t  rxBuffer[CHUNK_SIZE];

String cmdBuffer = "";

// ============================================================
//  FORWARD DECLARATIONS
// ============================================================
void processCommand(const String& cmd);
void executeRollback(char partitionIdentifier);

// ============================================================
//  SETUP
// ============================================================
void setup() {
  // UART0 is connected to Gateway UART2
  Serial.begin(UART_BAUD);
  
  // Identify current running partition for diagnostics
  const esp_partition_t* running = esp_ota_get_running_partition();
  // We do not print diagnostics to Serial here, as it would disrupt 
  // the Gateway's UART listener expecting protocol tokens.
}

// ============================================================
//  MAIN LOOP
// ============================================================
void loop() {
  while (Serial.available()) {
    
    if (currentState == STATE_COMMAND) {
      char c = (char)Serial.read();
      if (c == '\n') {
        cmdBuffer.trim();
        if (cmdBuffer.length() > 0) {
          processCommand(cmdBuffer);
        }
        cmdBuffer = "";
      } else if (c != '\r') {
        cmdBuffer += c;
      }
    } 
    
    else if (currentState == STATE_RECEIVING_OTA) {
      rxBuffer[chunkReceived++] = Serial.read();
      
      // Check if a full chunk (or the final partial chunk) is received
      if (chunkReceived >= chunkExpected) {
        
        // Write chunk to flash
        size_t written = Update.write(rxBuffer, chunkReceived);
        
        if (written == chunkReceived) {
          Serial.println("ACK");
          otaRemaining -= chunkReceived;
          
          if (otaRemaining == 0) {
            // Transfer complete, return to command mode to await OTA_DONE
            currentState = STATE_COMMAND;
            cmdBuffer = "";
          } else {
            // Prepare for next chunk
            chunkExpected = (otaRemaining > CHUNK_SIZE) ? CHUNK_SIZE : otaRemaining;
            chunkReceived = 0;
          }
        } else {
          // Flash write failed
          Serial.println("ERROR");
          Update.abort();
          currentState = STATE_COMMAND;
          cmdBuffer = "";
        }
      }
    }
  }
}

// ============================================================
//  PROTOCOL HANDLERS
// ============================================================
void processCommand(const String& cmd) {
  
  if (cmd.startsWith("START_OTA:")) {
    otaTotalSize = cmd.substring(10).toInt();
    
    if (otaTotalSize == 0 || otaTotalSize > UPDATE_SIZE_UNKNOWN) {
      Serial.println("ERROR");
      return;
    }

    // U_FLASH indicates writing to the next available OTA partition
    if (Update.begin(otaTotalSize, U_FLASH)) {
      otaRemaining  = otaTotalSize;
      chunkExpected = (otaRemaining > CHUNK_SIZE) ? CHUNK_SIZE : otaRemaining;
      chunkReceived = 0;
      currentState  = STATE_RECEIVING_OTA;
      
      Serial.println("READY");
    } else {
      Serial.println("ERROR");
    }
  } 
  
  else if (cmd == "OTA_DONE") {
    if (Update.end(true)) {
      Serial.println("OK");
      delay(100);
      ESP.restart(); // Reboot into the new firmware
    } else {
      Serial.println("ERROR");
    }
  } 
  
  else if (cmd.startsWith("ROLLBACK:")) {
    char partitionIdent = cmd.charAt(9);
    executeRollback(partitionIdent);
  }
}

// ============================================================
//  PARTITION MANAGEMENT
// ============================================================
void executeRollback(char partitionIdentifier) {
  esp_partition_subtype_t targetSubtype;
  
  if (partitionIdentifier == 'A') {
    targetSubtype = ESP_PARTITION_SUBTYPE_APP_OTA_0;
  } else if (partitionIdentifier == 'B') {
    targetSubtype = ESP_PARTITION_SUBTYPE_APP_OTA_1;
  } else {
    Serial.println("ERROR");
    return;
  }

  const esp_partition_t* partition = esp_partition_find_first(
    ESP_PARTITION_TYPE_APP, 
    targetSubtype, 
    NULL
  );

  if (partition != NULL) {
    esp_err_t err = esp_ota_set_boot_partition(partition);
    if (err == ESP_OK) {
      Serial.println("OK");
      delay(100);
      ESP.restart();
    } else {
      Serial.println("ERROR");
    }
  } else {
    Serial.println("ERROR");
  }
}