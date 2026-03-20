#if defined(ESP8266)
  #include <ESP8266WiFi.h>
  #include <espnow.h>
#elif defined(ESP32)
  #include <WiFi.h>
  #include <esp_now.h>
#endif

#include <OSCBundle.h>
#include <SLIPEncodedSerial.h>

#ifdef BOARD_HAS_USB_SERIAL
SLIPEncodedUSBSerial SLIPSerial( thisBoardsSerialUSB );
#else
SLIPEncodedSerial SLIPSerial(Serial);
#endif

#define SENSOR_DATA_LEN 5
#define SPAT_OFFSET UINT_MAX / 2

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

    OSCBundle bndl;
    bool spat = incomingPacket.deviceID > SPAT_OFFSET;
    unsigned int device_id = spat ? incomingPacket.deviceID - SPAT_OFFSET : incomingPacket.deviceID;
    String header = "/abmotion/" + String(device_id);

    String batteryHeader = header + "/b";
    bndl.add(batteryHeader.c_str()).add(incomingPacket.battery);

    if(!spat){
      String motionHeader = header + "/m";
      String rotationHeader = header + "/r";
      bndl.add(motionHeader.c_str()).add(incomingPacket.data[0]);
      bndl.add(rotationHeader.c_str()).add(incomingPacket.data[1])
                                      .add(incomingPacket.data[2])
                                      .add(incomingPacket.data[3])
                                      .add(incomingPacket.data[4]);
    }
    else{
      String distanceHeader = header + "/d";
      bndl.add(distanceHeader.c_str()).add(incomingPacket.data[0])
                                      .add(incomingPacket.data[1])
                                      .add(incomingPacket.data[2])
                                      .add(incomingPacket.data[3])
                                      .add(incomingPacket.data[4]);
    }

    SLIPSerial.beginPacket();
    bndl.send(SLIPSerial);
    SLIPSerial.endPacket();
    bndl.empty();
  }
}

void setup() {
  SLIPSerial.begin(115200);

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
    return;
  }

  esp_now_register_recv_cb(onReceive);
}

void loop() {
}