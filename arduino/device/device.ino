#include <WiFi.h>
#include <esp_now.h>
#include <CodeCell.h>

#define DEVICE_ID 17
#define SAMPLERATE 40 // Hz
#define SEND_RATE 20  // Hz

#define WAIT_N_FRAMES (SAMPLERATE / SEND_RATE)

#define RMS_WINDOW 100 // milliseconds
#define DEADZONE .1
#define LP_CUTOFF_FREQ 2.0f
#define MAX_FROZEN 5
#define RADIO_OFF_RESTART_MS (10UL * 60UL * 1000UL) // 10min in milliseconds
#define STILL_TIMEOUT_MS 30000                       // milliseconds

CodeCell myCodeCell;

uint8_t receiverMAC[] = {0xAC, 0xA7, 0x04, 0xD4, 0xDA, 0x88};

#define SENSOR_DATA_LEN 8

typedef struct __attribute__((packed)) {
  unsigned int deviceID;
  unsigned int battery;
  float data[SENSOR_DATA_LEN];
} DataPacket;

#define RMS_WINDOW_SAMPS ((int)((float)RMS_WINDOW / 1000.0f * SAMPLERATE))

float rms_buffer[RMS_WINDOW_SAMPS] = {0.0f};
int rms_head = 0;
float rms_sum_sq = 0.0f;

float rms = 0.0f;
float rms_prev = 0.0f;
float lp = 0.0f;
float lp_alpha = 0.0f;
int frozen_frames = 0;

float interval_max_delta = 0.0f; // max RMS rise within the current send interval
float interval_peak_rms = 0.0f;  // max RMS within the current send interval

int frame_counter = 1; // don't send packet immediately on boot
bool radio_on = true;
unsigned long still_since = 0;
unsigned long radioOffSince = 0; // tracks when radio was turned off

bool is_idle = false;
bool is_charging = false;

float ax = 0, ay = 0, az = 0;
float qw = 0, qx = 0, qy = 0, qz = 0;
float ax_hist = 0, ay_hist = 0, az_hist = 0;
float qw_hist = 1, qx_hist = 0, qy_hist = 0, qz_hist = 0;

DataPacket packet;

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

  rms_prev = rms;
  rms = sqrt(rms_sum_sq / RMS_WINDOW_SAMPS);
  rms *= (rms >= DEADZONE);

  float delta = rms - rms_prev;
  if (delta > interval_max_delta) interval_max_delta = delta;
  if (rms > interval_peak_rms) interval_peak_rms = rms;

  float new_lp = rms * lp_alpha + (1.0f - lp_alpha) * lp;
  if (isfinite(new_lp)) lp = new_lp;
}

bool freeze_check() {
  if (ax == ax_hist && ay == ay_hist && az == az_hist &&
      qw == qw_hist && qx == qx_hist && qy == qy_hist && qz == qz_hist) {

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

void setup() {
  Serial.begin(115200);

  myCodeCell.Init(MOTION_LINEAR_ACC + MOTION_ROTATION);
  myCodeCell.LED_SetBrightness(0);

  lp_alpha = calc_lp_alpha(LP_CUTOFF_FREQ, SAMPLERATE);

  radio_start();

  packet.deviceID = DEVICE_ID;
}

void loop() {
  if (myCodeCell.Run(SAMPLERATE)) {
    myCodeCell.LED_SetBrightness(0);

    if (!radio_on && radioOffSince > 0 && millis() - radioOffSince >= RADIO_OFF_RESTART_MS) {
      ESP.restart();
    }

    charging_check();

    calc_motion_scalar();
    idle_check();

    qw_hist = qw;
    qx_hist = qx;
    qy_hist = qy;
    qz_hist = qz;
    myCodeCell.Motion_RotationVectorRead(qw, qx, qy, qz);

    freeze_check();

    if (is_charging || is_idle) {
      if (radio_on) radio_stop();
      return;
    } else if (!is_charging && !is_idle && !radio_on) {
      radio_start();
    }

    if (frame_counter == 0 && radio_on) {
      packet.data[0] = lp;
      packet.data[1] = qw;
      packet.data[2] = qx;
      packet.data[3] = qy;
      packet.data[4] = qz;
      packet.data[5] = interval_max_delta;
      packet.data[6] = interval_peak_rms;

      interval_max_delta = 0.0f;
      interval_peak_rms = 0.0f;

      packet.battery = (unsigned int)min((int)myCodeCell.BatteryLevelRead(), 100);

      esp_err_t result = esp_now_send(
        receiverMAC,
        (uint8_t*)&packet,
        sizeof(packet)
      );
    }

    frame_counter = (frame_counter + 1) % WAIT_N_FRAMES;
  }
}