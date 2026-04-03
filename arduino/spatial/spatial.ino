// Uses code by Jaryd from Core Electronics
// https://core-electronics.com.au/guides/sensors/diy-2d-and-3d-spatial-tracking-with-ultra-wideband-arduino-and-pico-guide/#XC4IICX

#include <WiFi.h>
#include <esp_now.h>
#include <CodeCell.h>

#define DEVICE_ID 9

#define RX_PIN 21
#define TX_PIN 22

#define SAMPLERATE 20 // Hz

#define RMS_WINDOW 1000 // milliseconds (substantially higher for torso sensor due to gait)
#define DEADZONE .1
#define LP_CUTOFF_FREQ 2.0f
#define MAX_FROZEN 5
#define RADIO_OFF_RESTART_MS (10UL * 60UL * 1000UL)
#define STILL_TIMEOUT_MS 30000

uint8_t receiverMAC[] = {0xAC, 0xA7, 0x04, 0xD4, 0xDA, 0x88};

#define SENSOR_DATA_LEN 8
#define ID_OFFSET UINT_MAX / 2

typedef struct __attribute__((packed)) {
  unsigned int deviceID;
  unsigned int battery;
  float data[SENSOR_DATA_LEN];
} DataPacket;

DataPacket uwbPacket;
DataPacket motionPacket;

CodeCell myCodeCell;

bool radio_on = true;
unsigned long radioOffSince = 0;

bool send_data = true;
bool is_idle = false;
bool is_charging = false;

#define RMS_WINDOW_SAMPS ((int)((float)RMS_WINDOW / 1000.0f * SAMPLERATE))

float rms_buffer[RMS_WINDOW_SAMPS] = {0.0f};
int rms_head = 0;
float rms_sum_sq = 0.0f;
float rms = 0.0;
float lp = 0.0;
float lp_alpha = 0.0;

unsigned long still_since = 0;

float ax = 0, ay = 0, az = 0;
float ax_hist = 0, ay_hist = 0, az_hist = 0;
int frozen_frames = 0;

float calc_lp_alpha(float cutoff_freq, float sample_rate) {
  float rc = 1.0f / (2.0f * PI * cutoff_freq);
  float dt = 1.0f / sample_rate;
  return dt / (rc + dt);
}

void radio_start() {
  WiFi.mode(WIFI_STA);
  esp_now_init();

  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, receiverMAC, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;

  esp_now_add_peer(&peerInfo);
  radio_on = true;
  radioOffSince = 0;
}

void radio_stop() {
  esp_now_deinit();
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  radio_on = false;
  if (radioOffSince == 0) radioOffSince = millis();
}

void calc_motion_scalar() {
  ax_hist = ax;
  ay_hist = ay;
  az_hist = az;

  myCodeCell.Motion_LinearAccRead(ax, ay, az);

  float new_val = ax * ax + ay * ay + az * az;
  if (!isfinite(new_val)) new_val = 0.0f;

  rms_sum_sq -= rms_buffer[rms_head];
  rms_buffer[rms_head] = new_val;
  rms_sum_sq += new_val;
  rms_head = (rms_head + 1) % RMS_WINDOW_SAMPS;

  if (rms_sum_sq < 0.0f) rms_sum_sq = 0.0f;

  rms = sqrt(rms_sum_sq / RMS_WINDOW_SAMPS);
  rms *= (rms >= DEADZONE);

  float new_lp = rms * lp_alpha + (1.0f - lp_alpha) * lp;
  if (isfinite(new_lp)) lp = new_lp;
}

bool freeze_check() {
  if (ax == ax_hist && ay == ay_hist && az == az_hist) {
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

void charging_check() {
  if (myCodeCell.PowerStateRead() == 5) {
    myCodeCell.LED_SetBrightness(2);
    myCodeCell.LED_Breathing(LED_COLOR_GREEN);
    is_charging = true;
  } else {
    is_charging = false;
  }
}

void idle_check() {
  if (rms < DEADZONE) {
    if (still_since == 0) still_since = millis();
    if (millis() - still_since >= STILL_TIMEOUT_MS) is_idle = true;
  } else {
    still_since = 0;
    is_idle = false;
  }
}

bool decodeUwbDistances(uint8_t* data, int dataLen, float* distances) {
  for (int i = 0; i < SENSOR_DATA_LEN; i++) distances[i] = -1.0;

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

void setup() {
  Serial.begin(115200);

  myCodeCell.Init(MOTION_LINEAR_ACC);
  myCodeCell.LED_SetBrightness(0);

  Serial1.begin(115200, SERIAL_8N1, RX_PIN, TX_PIN);

  lp_alpha = calc_lp_alpha(LP_CUTOFF_FREQ, SAMPLERATE);

  radio_start();

  uwbPacket.deviceID = DEVICE_ID + ID_OFFSET;
  motionPacket.deviceID = DEVICE_ID;
}

void loop() {
  if (myCodeCell.Run(SAMPLERATE)) {
    myCodeCell.LED_SetBrightness(0);

    // Restart if radio has been off too long
    if (!radio_on && radioOffSince > 0 && millis() - radioOffSince >= RADIO_OFF_RESTART_MS) {
      ESP.restart();
    }

    charging_check();
    calc_motion_scalar();
    idle_check();
    freeze_check();

    if (is_charging || is_idle) {
      if (radio_on) radio_stop();
      return;
    } else if (!is_charging && !is_idle && !radio_on) {
      radio_start();
    }

    // Read and decode incoming UWB serial data
    static uint8_t buffer[256];
    static int bufferIndex = 0;
    static bool messageStarted = false;
    static float latestDistances[SENSOR_DATA_LEN] = {-1.0};
    static bool hasValidData = false;

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
          messageStarted = false;
          bufferIndex = 0;
        }

        if (bufferIndex >= 256) {
          messageStarted = false;
          bufferIndex = 0;
        }
      }
    }

    if (send_data && radio_on) {
      unsigned int batt = (unsigned int)min((int)myCodeCell.BatteryLevelRead(), 100);

      // Send UWB distances packet
      if (hasValidData) {
        uwbPacket.battery = batt;
        memcpy(uwbPacket.data, latestDistances, sizeof(latestDistances));
        esp_now_send(receiverMAC, (uint8_t*)&uwbPacket, sizeof(uwbPacket));
      }

      // Send motion scalar packet (no ID offset)
      motionPacket.battery = batt;
      motionPacket.data[0] = lp;
      esp_now_send(receiverMAC, (uint8_t*)&motionPacket, sizeof(motionPacket));
    }

    send_data = !send_data;
  }
}