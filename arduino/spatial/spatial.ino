#include <WiFi.h>
#include <esp_now.h>
#include <CodeCell.h>

#define DEVICE_ID 18

#define RX_PIN 21
#define TX_PIN 22

#define SEND_RATE 10 // Send every 100ms

//AC:A7:04:D4:DA:88
uint8_t receiverMAC[] = {0xAC, 0xA7, 0x04, 0xD4, 0xDA, 0x88};

#define SENSOR_DATA_LEN 5
#define ID_OFFSET UINT_MAX / 2

typedef struct __attribute__((packed)) {
  unsigned int deviceID;
  unsigned int battery;
  float data[SENSOR_DATA_LEN];
} DataPacket;

DataPacket packet;

CodeCell myCodeCell;

void setup() {
  Serial.begin(115200);

  myCodeCell.Init(0);
  myCodeCell.LED_SetBrightness(0);

  Serial1.begin(115200, SERIAL_8N1, RX_PIN, TX_PIN);

  WiFi.mode(WIFI_STA);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    return;
  }

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, receiverMAC, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;

  esp_now_add_peer(&peerInfo);

  packet.deviceID = DEVICE_ID + ID_OFFSET;
  //packet.battery = 0;
}

bool decodeUwbDistances(uint8_t* data, int dataLen, float* distances) {
  for (int i = 0; i < SENSOR_DATA_LEN; i++) {
    distances[i] = -1.0;
  }

  if (dataLen < 35) return false;
  if (data[0] != 0xaa || data[1] != 0x25 || data[2] != 0x01) return false;

  for (int i = 0; i < SENSOR_DATA_LEN; i++) {
    int byteOffset = 3 + (i * 4);
    if (byteOffset + 3 < dataLen) {
      uint32_t distanceRaw = data[byteOffset] |
                            (data[byteOffset + 1] << 8) |
                            (data[byteOffset + 2] << 16) |
                            (data[byteOffset + 3] << 24);

      distances[i] = (distanceRaw > 0) ? distanceRaw / 1000.0 : -1.0;
    }
  }

  return true;
}

void printDistances(float* distances, bool validData) {
  if (!validData) {
    Serial.println("Invalid data received");
    return;
  }

  Serial.println("Base Station Distances:");
  for (int i = 0; i < SENSOR_DATA_LEN; i++) {
    Serial.print("  BS");
    Serial.print(i);
    if (distances[i] > 0) {
      Serial.print(": ");
      Serial.print(distances[i], 3);
      Serial.println("m");
    } else {
      Serial.println(": Not visible");
    }
  }
  Serial.println("------------------------------");
}

void loop() {
  static uint8_t buffer[256];
  static int bufferIndex = 0;
  static bool messageStarted = false;
  static float latestDistances[SENSOR_DATA_LEN] = {-1.0};
  static bool hasValidData = false;
  static unsigned long lastSendTime = 0;

  // Read and decode incoming UWB serial data
  while (Serial1.available()) {
    uint8_t incomingByte = Serial1.read();

    if (!messageStarted && incomingByte == 0xAA) {
      messageStarted = true;
      bufferIndex = 0;
      buffer[bufferIndex++] = incomingByte;
    } else if (messageStarted) {
      buffer[bufferIndex++] = incomingByte;

      if (bufferIndex >= 35) {
        hasValidData = decodeUwbDistances(buffer, bufferIndex, latestDistances);
        printDistances(latestDistances, hasValidData);

        messageStarted = false;
        bufferIndex = 0;
      }

      if (bufferIndex >= 256) {
        messageStarted = false;
        bufferIndex = 0;
      }
    }
  }

  // Send latest distances at SEND_RATE
  unsigned long now = millis();
  if (now - lastSendTime >= 1000.0f / SEND_RATE) {
    lastSendTime = now;

    if(myCodeCell.Run(SEND_RATE)){
      packet.battery = (unsigned int)myCodeCell.BatteryLevelRead();
    }

    if (hasValidData) {
      memcpy(packet.data, latestDistances, sizeof(latestDistances));
      esp_now_send(receiverMAC, (uint8_t*)&packet, sizeof(packet));
    }
  }
}