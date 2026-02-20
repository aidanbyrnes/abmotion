#include <WiFi.h>
#include <esp_now.h>
#include <CodeCell.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <Quaternion.h>

#define DEVICE_ID 1
#define SAMPLERATE 20 // Hz
#define SEND_RATE 10  // Hz

#define RMS_WINDOW 200 // milliseconds
#define DEADZONE .1
#define LP_CUTOFF_FREQ 2.0f
#define MAX_FROZEN 5

CodeCell myCodeCell;

uint8_t receiverMAC[] = {0xBC, 0xDD, 0xC2, 0x2F, 0x5A, 0x24};

#define SENSOR_DATA_LEN 5 // 3 slots reserved for future use

typedef struct __attribute__((packed)) {
  unsigned int deviceID;
  unsigned int battery;
  float data[SENSOR_DATA_LEN];
} DataPacket;

TaskHandle_t sensorTaskHandle = NULL;
TaskHandle_t espnowTaskHandle = NULL;
QueueHandle_t sensorDataQueue;

float rms_accum = 0.0;
float rms = 0.0;
float lp = 0.0;
float lp_alpha = 0.0;
int rms_index = 0;
int rms_samps;
int frozen_frames = 0;

float ax = 0, ay = 0, az = 0;
float ax_hist = 0, ay_hist = 0, az_hist = 0;
float qw_hist = 1, qx_hist = 0, qy_hist = 0, qz_hist = 0;

Quaternion quat;

float calc_lp_alpha(float cutoff_freq, float sample_rate) {
  float rc = 1.0f / (2.0f * PI * cutoff_freq);
  float dt = 1.0f / sample_rate;
  return dt / (rc + dt);
}

int calc_buffer_size(int sr) {
  int size = (float)RMS_WINDOW / 1000.0f * sr;
  return (size > 0) ? size : 1;
}

void calc_motion_scalar(int sr = SAMPLERATE) {
  ax_hist = ax;
  ay_hist = ay;
  az_hist = az;

  myCodeCell.Motion_LinearAccRead(ax, ay, az);

  rms_accum += ax * ax + ay * ay + az * az;
  rms_index++;
  rms_samps = calc_buffer_size(sr);

  if (rms_index >= rms_samps) {
    rms = sqrt(rms_accum / rms_samps);
    rms *= (rms >= DEADZONE);
    rms_index = 0;
    rms_accum = 0.0;
  }

  lp = rms * lp_alpha + (1.0f - lp_alpha) * lp;
}

bool freeze_check() {
  if (ax == ax_hist && ay == ay_hist && az == az_hist &&
      quat.w == qw_hist && quat.x == qx_hist &&
      quat.y == qy_hist && quat.z == qz_hist) {

    if (frozen_frames > MAX_FROZEN) {
      Serial.println(">> Freeze detected");
      ESP.restart();
    } else {
      frozen_frames++;
      return true;
    }
  } else {
    frozen_frames = 0;
  }

  return false;
}

void sensorTask(void *parameter) {

  const unsigned int send_interval = SAMPLERATE / SEND_RATE;
  unsigned int sample_count = 0;

  DataPacket packet;
  packet.deviceID = DEVICE_ID;

  while (1) {

    if (myCodeCell.Run(SAMPLERATE)) {

      calc_motion_scalar();

      qw_hist = quat.w;
      qx_hist = quat.x;
      qy_hist = quat.y;
      qz_hist = quat.z;
      myCodeCell.Motion_RotationVectorRead(quat.w, quat.x, quat.y, quat.z);

      if (freeze_check()) continue;

      sample_count++;

      if (sample_count >= send_interval) {

        packet.data[0] = lp;

        packet.data[1] = quat.w;
        packet.data[2] = quat.x;
        packet.data[3] = quat.y;
        packet.data[4] = quat.z;

        packet.battery = (unsigned int)myCodeCell.BatteryLevelRead();

        xQueueSend(sensorDataQueue, &packet, 0);

        sample_count = 0;
      }
    }
  }
}

void espnowTask(void *parameter) {

  DataPacket packet;

  while (1) {

    if (xQueueReceive(sensorDataQueue, &packet, portMAX_DELAY) == pdTRUE) {

      esp_err_t result = esp_now_send(
        receiverMAC,
        (uint8_t*)&packet,
        sizeof(packet)
      );

      if (result != ESP_OK) {
        Serial.printf(">> Send failed: %d\n", result);
      }
    }
  }
}

void setup() {

  Serial.begin(115200);

  myCodeCell.Init(MOTION_LINEAR_ACC + MOTION_ROTATION);
  myCodeCell.LED_SetBrightness(0);

  lp_alpha = calc_lp_alpha(LP_CUTOFF_FREQ, SAMPLERATE);

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

  sensorDataQueue = xQueueCreate(2, sizeof(DataPacket));

  xTaskCreate(sensorTask, "SensorTask", 4096, NULL, 2, &sensorTaskHandle);
  xTaskCreate(espnowTask, "ESPNowTask", 4096, NULL, 1, &espnowTaskHandle);
}

void loop() {
  vTaskDelay(portMAX_DELAY);
}