#include <WiFi.h>
#include <esp_now.h>
#include <CodeCell.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

#define DEVICE_ID 1
#define SAMPLERATE 30 //Hz

#define SEND_RATE 10 //Hz

#define RMS_WINDOW 100 //milliseconds
#define DEADZONE .1
#define LP_CUTOFF_FREQ 2.0f  // Low-pass cutoff frequency in Hz
#define SLEEP_TIMER 5 // How many seconds to wait until sleep when device is active
#define MAX_FROZEN 5 // How many consecutive frames are allowed to be the same before sensor is considered frozen

CodeCell myCodeCell;

uint8_t receiverMAC[] = {0x28, 0x37, 0x2F, 0xC9, 0x04, 0x24};  // <-- change to receiver MAC

typedef struct __attribute__((packed)) {
  unsigned int deviceID;
  float motion_scalar;
  float qw;
  float qx;
  float qy;
  float qz;
  unsigned int battery;
} DataPacket;

typedef struct {
  float motion;
  float qw;
  float qx;
  float qy;
  float qz;
  unsigned int battery;
} SensorData;

TaskHandle_t sensorTaskHandle = NULL;
TaskHandle_t espnowTaskHandle = NULL;

QueueHandle_t sensorDataQueue;

// motion processing
float motion_hist;
int rms_index = 0;
int rms_samps;
float rms_accum = 0.0;
float rms = 0.0;
float lp_hist = 0.0;
float lp = 0.0;
float lp_alpha = 0.0;
int stationary_frames = 0;
int frozen_frames = 0;

float ax = 0, ay = 0, az = 0;
float qw = 1, qx = 0, qy = 0, qz = 0;
float ax_hist = 0, ay_hist = 0, az_hist = 0;
float qw_hist = 1, qx_hist = 0, qy_hist = 0, qz_hist = 0;

float calc_lp_alpha(float cutoff_freq, float sample_rate) {
  float rc = 1.0f / (2.0f * PI * cutoff_freq);
  float dt = 1.0f / sample_rate;
  return dt / (rc + dt);
}

int calc_buffer_size(int sr){
  int size = (float)RMS_WINDOW / 1000.0f * sr;
  size = (size > 0) ? size : 1;
  return size;
}

float calc_motion_scalar(int sr = SAMPLERATE){
  ax_hist = ax;
  ay_hist = ay;
  az_hist = az;

  myCodeCell.Motion_LinearAccRead(ax, ay, az);
  rms_accum += ax * ax + ay * ay + az * az; //magnitude of accel squared

  rms_index++;
  rms_samps = calc_buffer_size(sr);
  if(rms_index == rms_samps){
    rms = sqrt(rms_accum / rms_samps);
    rms *= (rms >= DEADZONE);
    rms_index = 0;
    rms_accum = 0.0;
  }

  lp_hist = lp;
  lp = rms * lp_alpha + (1 - lp_alpha) * lp_hist;

  return lp;
}

void freeze_check(){
    if(ax == ax_hist && ay == ay_hist && az == az_hist && qw == qw_hist && qx == qx_hist && qy == qy_hist && qz == qz_hist){
      frozen_frames++;
      if(frozen_frames > MAX_FROZEN){
        Serial.println(">> Freeze detected");
        ESP.restart();
      }
    }
    else{
      frozen_frames = 0;
    }
}

void sensorTask(void *parameter) {
  TickType_t xLastWakeTime = xTaskGetTickCount();
  
  unsigned int sample_count = 0;
  const unsigned int send_interval = SAMPLERATE / SEND_RATE;
  
  SensorData data;
  
  while(1) {
    if(myCodeCell.Run(SAMPLERATE)) {
      motion_hist = data.motion;
      data.motion = calc_motion_scalar();

      qw_hist = qw;
      qx_hist = qx;
      qy_hist = qy;
      qz_hist = qz;
      myCodeCell.Motion_RotationVectorRead(qw, qx, qy, qz);

      // Check for frozen data
      freeze_check();

      // Check for sleep condition is device is still
      // if(data.motion < 0.001 || data.motion == motion_hist){
      //   if((float)stationary_frames / SAMPLERATE >= SLEEP_TIMER + RMS_WINDOW / 1000.){
      //     myCodeCell.SleepTimer(0);
      //   }
      //   else{
      //     stationary_frames++;
      //   }
      // }
      // else{
      //   stationary_frames = 0;
      // }
      
      sample_count++;
      
      if(sample_count >= send_interval) {
        data.qw = qw;
        data.qx = qx;
        data.qy = qy;
        data.qz = qz;

        data.battery = (unsigned int)myCodeCell.BatteryLevelRead();
        
        xQueueSend(sensorDataQueue, &data, 0);
        
        sample_count = 0;
      }
    }
  }
}

void espnowTask(void *parameter) {
  SensorData data;
  DataPacket packet;
  packet.deviceID = DEVICE_ID;
  
  while(1) {
    if(xQueueReceive(sensorDataQueue, &data, portMAX_DELAY) == pdTRUE) {
      packet.motion_scalar = data.motion;
      packet.qw = data.qw;
      packet.qx = data.qx;
      packet.qy = data.qy;
      packet.qz = data.qz;
      packet.battery = data.battery;
      
      esp_err_t result = esp_now_send(receiverMAC, (uint8_t*)&packet, sizeof(packet));
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

  // // Wait for some initial motion on startup
  // Serial.println(">> Waiting for motion..");
  // int init_sr = 10;
  // // Run once to update sensors
  // myCodeCell.Run(init_sr);
  // float init_motion = 0.0;
  // while(init_motion == 0.0){
  //   if(myCodeCell.Run(init_sr)){
  //     init_motion = calc_motion_scalar(init_sr);
  //   }
  // }
  // Serial.println(">> Motion detected");

  WiFi.mode(WIFI_STA);

  if (esp_now_init() != ESP_OK) {
    Serial.println("ESP-NOW init failed");
    return;
  }

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, receiverMAC, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;

  esp_now_add_peer(&peerInfo);

  sensorDataQueue = xQueueCreate(2, sizeof(SensorData));

  xTaskCreate(
    sensorTask,
    "SensorTask",
    4096,
    NULL,
    2,
    &sensorTaskHandle
  );

  xTaskCreate(
    espnowTask,
    "ESPNowTask",
    4096,
    NULL,
    1,
    &espnowTaskHandle
  );
}

void loop() {
  vTaskDelay(portMAX_DELAY);
}