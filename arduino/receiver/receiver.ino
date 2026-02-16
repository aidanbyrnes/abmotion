#include <WiFi.h>
#include <esp_now.h>

typedef struct __attribute__((packed)) {
  unsigned int deviceID;
  float accelMag;
  float qw;
  float qx;
  float qy;
  float qz;
  unsigned int battery;
} DataPacket;

DataPacket incomingPacket;

void onReceive(const esp_now_recv_info_t *info, const uint8_t *data, int len) {

  if (len == sizeof(DataPacket)) {
    memcpy(&incomingPacket, data, sizeof(DataPacket));
    Serial.print(incomingPacket.deviceID); Serial.print(" ");
    Serial.print(incomingPacket.accelMag); Serial.print(" ");
    Serial.print(incomingPacket.qw); Serial.print(" ");
    Serial.print(incomingPacket.qx); Serial.print(" ");
    Serial.print(incomingPacket.qy); Serial.print(" ");
    Serial.print(incomingPacket.qz); Serial.print(" ");
    Serial.println(incomingPacket.battery);
  }
}

void setup() {
  Serial.begin(115200);

  WiFi.mode(WIFI_STA);
  Serial.println(WiFi.macAddress());

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    return;
  }

  esp_now_register_recv_cb(onReceive);
}

void loop() {
}