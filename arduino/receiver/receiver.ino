#if defined(ESP8266)
  #include <ESP8266WiFi.h>
  #include <espnow.h>
#elif defined(ESP32)
  #include <WiFi.h>
  #include <esp_now.h>
#endif

#define SENSOR_DATA_LEN 5

typedef struct __attribute__((packed)) {
  unsigned int deviceID;
  unsigned int battery;
  float data[SENSOR_DATA_LEN];
} DataPacket;

DataPacket incomingPacket;

#if defined(ESP8266)
void onReceive(uint8_t *mac, uint8_t *data, uint8_t len) {
#elif defined(ESP32)
void onReceive(const esp_now_recv_info_t *info, const uint8_t *data, int len) {
#endif
  if (len == sizeof(DataPacket)) {
    memcpy(&incomingPacket, data, sizeof(DataPacket));

    Serial.print(incomingPacket.deviceID); Serial.print(" ");
    Serial.print(incomingPacket.battery);  Serial.print(" ");

    for (int i = 0; i < SENSOR_DATA_LEN; i++) {
      if (i < SENSOR_DATA_LEN - 1) {
        Serial.print(incomingPacket.data[i]); Serial.print(" ");
      } else {
        Serial.println(incomingPacket.data[i]);
      }
    }
  }
}

void setup() {
  Serial.begin(115200);

  WiFi.mode(WIFI_STA);
#if defined(ESP8266)
  delay(1000);
#endif
  Serial.println(WiFi.macAddress());

#if defined(ESP8266)
  if (esp_now_init() != 0) {
#elif defined(ESP32)
  if (esp_now_init() != ESP_OK) {
#endif
    Serial.println("ESP-NOW init failed");
    return;
  }

  esp_now_register_recv_cb(onReceive);
}

void loop() {
}