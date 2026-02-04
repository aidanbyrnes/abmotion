#include <WiFi.h>
#include <WiFiUdp.h>
#include <ESPmDNS.h>
#include <OSCBundle.h>
#include <CodeCell.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

// Network config
#define SSID "abwireless"
#define PSWD "abwireless"
#define HOSTNAME "abmac"

#define ID 1
#define SAMPLERATE 20 // 20Hz seems to be stable
#define RMS_WINDOW 100 //milliseconds

#define DEADZONE .1
#define HP_ALPHA .9f
#define LP_CUTOFF_FREQ 2.0f  // Low-pass cutoff frequency in Hz

#define OSC_ENABLE true
#define OSC_RATE 10 //Hz

#define WAKE_TIMER 30 //how many seconds to wait for movement on device wake
#define SLEEP_TIMER 10 //how many seconds to wait until sleep when device is active

// OSC destination
String host;
const int destPort = 8000 + ID;

WiFiUDP Udp;
CodeCell myCodeCell;

// Shared data
struct SensorData {
  float motion_scalar;
  float qw, qx, qy, qz;
  int battery;
  bool data_updated;
};

// Global variables
SensorData currentData = {0, 0, 0, 0, 0, 0, false};
SensorData lastSentData = {0, 0, 0, 0, 0, 0, false};

// FreeRTOS handles
SemaphoreHandle_t dataMutex;
TaskHandle_t sensorTaskHandle = NULL;
TaskHandle_t wifiTaskHandle = NULL;

// Sensor processing variables
int rms_index = 0;
int rms_samps;
float rms_accum = 0.0;
float rms = 0.0;
float lp_hist = 0.0;
float lp = 0.0;
float lp_alpha = 0.0;
int stationary_frames = 0;

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
  float x, y, z;
  myCodeCell.Motion_LinearAccRead(x, y, z);
  rms_accum += x * x + y * y + z * z; //magnitude of accel squared

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

void print_data(SensorData &data){
    Serial.print(">> ID: ");
    Serial.print(ID);
    Serial.print(", RMS samples: ");
    Serial.print(rms_samps);
    Serial.print(", Sleep Timer: ");
    float timer = (float)stationary_frames / SAMPLERATE - RMS_WINDOW / 1000.0;
    timer = (timer > 0.0) ? SLEEP_TIMER - timer : SLEEP_TIMER;
    Serial.print(timer);
    Serial.print(", Motion: ");
    Serial.print(data.motion_scalar);
    Serial.print(", Rotation: "); 
    Serial.print(data.qw); Serial.print(" "); 
    Serial.print(data.qx); Serial.print(" "); 
    Serial.print(data.qy); Serial.print(" "); 
    Serial.println(data.qz);
}

void getHost(){
  host = MDNS.queryHost(HOSTNAME).toString();
  if(host == "0.0.0.0"){
    Serial.println(">> Could not connect to host");
    myCodeCell.SleepTimer(5);
  }
}

void sensorTask(void *parameter) {
  TickType_t xLastWakeTime = xTaskGetTickCount();
  const TickType_t xFrequency = pdMS_TO_TICKS(1000 / SAMPLERATE);
  
  float motion_scalar_hist = 0.0;
  float motion_scalar_local = 0.0;
  
  while(1) {
    if (myCodeCell.Run(SAMPLERATE)) {
      motion_scalar_hist = motion_scalar_local;
      motion_scalar_local = calc_motion_scalar();

      // Check for sleep condition
      if(motion_scalar_local == 0 || motion_scalar_local == motion_scalar_hist){
        if((float)stationary_frames / SAMPLERATE >= SLEEP_TIMER + RMS_WINDOW / 1000.){
          myCodeCell.SleepTimer(1);
        }
        else{
          stationary_frames++;
        }
      }
      else{
        stationary_frames = 0;
      }

      // Update shared data with mutex protection
      if(xSemaphoreTake(dataMutex, portMAX_DELAY) == pdTRUE) {
        currentData.motion_scalar = motion_scalar_local;
        
        myCodeCell.Motion_RotationVectorRead(
          currentData.qw, 
          currentData.qx, 
          currentData.qy, 
          currentData.qz
        );
        
        currentData.battery = myCodeCell.BatteryLevelRead();
        
        currentData.data_updated = true;
        xSemaphoreGive(dataMutex);
      }
    }
    
    // Wait for next cycle (maintains consistent sample rate)
    vTaskDelayUntil(&xLastWakeTime, xFrequency);
  }
}

// WiFi/OSC Task
void wifiTask(void *parameter) {
  TickType_t xLastWakeTime = xTaskGetTickCount();
  const TickType_t xFrequency = pdMS_TO_TICKS(1000 / OSC_RATE);
  
  SensorData localData;
  
  while(1) {
    // Copy data with mutex protection
    if(xSemaphoreTake(dataMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
      if(currentData.data_updated) {
        localData = currentData;
        currentData.data_updated = false;
      }
      xSemaphoreGive(dataMutex);
    }

    if(OSC_ENABLE && WiFi.status() == WL_CONNECTED){
      OSCBundle bndl;

      // Send motion scalar if changed
      if(localData.motion_scalar != lastSentData.motion_scalar){
        bndl.add("/m").add(localData.motion_scalar);
        lastSentData.motion_scalar = localData.motion_scalar;
      }

      // Send rotation if changed
      if(localData.qw != lastSentData.qw || 
         localData.qx != lastSentData.qx || 
         localData.qy != lastSentData.qy || 
         localData.qz != lastSentData.qz){
        bndl.add("/r").add(localData.qw).add(localData.qx).add(localData.qy).add(localData.qz);
        lastSentData.qw = localData.qw;
        lastSentData.qx = localData.qx;
        lastSentData.qy = localData.qy;
        lastSentData.qz = localData.qz;
      }

      // Send battery if changed
      if(localData.battery != lastSentData.battery){
        bndl.add("/b").add((int32_t)localData.battery);
        lastSentData.battery = localData.battery;
      }

      // Send packet if it has messages
      if(bndl.size() > 0){
        Udp.beginPacket(host.c_str(), destPort);
        bndl.send(Udp);
        Udp.endPacket();
      }
    }

    print_data(localData);
    
    // Wait for next cycle
    vTaskDelayUntil(&xLastWakeTime, xFrequency);
  }
}

void setup() {
  Serial.begin(115200);
  myCodeCell.Init(MOTION_LINEAR_ACC + MOTION_ROTATION);
  myCodeCell.LED_SetBrightness(0);

  // Calculate low-pass alpha based on cutoff frequency and sample rate
  lp_alpha = calc_lp_alpha(LP_CUTOFF_FREQ, SAMPLERATE);
  Serial.print(">> LP Alpha (from ");
  Serial.print(LP_CUTOFF_FREQ);
  Serial.print(" Hz): ");
  Serial.println(lp_alpha);

  // Wait for some initial motion on startup before trying for WiFi
  int init_sr = 10; //samplerate for init

  Serial.println(">> Waiting for motion...");
  Serial.print(">> RMS samples: ");
  Serial.println(calc_buffer_size(init_sr));
  while(calc_motion_scalar(init_sr) == 0.0){
    if(!myCodeCell.Run(init_sr)){continue;}
    if(millis() / 1000 >= WAKE_TIMER){
      Serial.println(">> No motion detected");
      myCodeCell.SleepTimer(1);
    }
  }
  Serial.println(">> Motion detected");

  // Attempt to connect to WiFi
  WiFi.setSleep(false);
  Wire.setTimeOut(5000);
  WiFi.begin(SSID, PSWD);
  Serial.print(">> ");
  while (WiFi.status() != WL_CONNECTED) {
    if(millis() / 1000 >= WAKE_TIMER){
      Serial.println(">> Could not connect to WiFi");
      myCodeCell.SleepTimer(5);
    }
    delay(500);
    Serial.print(".");
  }
  Serial.println(WiFi.localIP());

  // Device will be discoverable as 'abmotion[ID]' on WiFi
  if (!MDNS.begin("abmotion" + String(ID))) {
    Serial.println(">> Error starting mDNS");
    return;
  }
  Serial.println(">> mDNS responder started");

  getHost();
  Serial.println(">> Host: " + host);

  Udp.begin(9000);

  // Create mutex for data protection
  dataMutex = xSemaphoreCreateMutex();
  if(dataMutex == NULL) {
    Serial.println(">> Failed to create mutex!");
    return;
  }

  xTaskCreate(
    sensorTask,           // Task function
    "SensorTask",         // Name
    4096,                 // Stack size
    NULL,                 // Parameters
    2,                    // Priority (higher)
    &sensorTaskHandle     // Task handle
  );

  xTaskCreate(
    wifiTask,             // Task function
    "WiFiTask",           // Name
    4096,                 // Stack size
    NULL,                 // Parameters
    1,                    // Priority (lower)
    &wifiTaskHandle       // Task handle
  );

  Serial.println(">> FreeRTOS tasks created");
}

void loop() {
  vTaskDelay(portMAX_DELAY);
}
