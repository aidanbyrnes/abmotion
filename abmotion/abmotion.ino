#include <WiFi.h>
#include <WiFiUdp.h>
#include <ESPmDNS.h>
#include <OSCBundle.h>
#include <CodeCell.h>

// Network config
#define SSID "abwireless"
#define PSWD "abwireless"

#define ID 1
#define SAMPLERATE 50 // 20Hz seems to be stable
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
int osc_timer = 0;

WiFiUDP Udp;

CodeCell myCodeCell;

float motion_scalar;
float motion_scalar_hist;
float motion_scalar_hist_osc;

int rms_index;
int rms_samps;
float rms_accum;
float rms = 0.0;

float lp_hist = 0.0;
float lp = 0.0;
float lp_alpha = 0.0;  // Will be calculated from frequency

float qw, qx, qy, qz;
float qw_hist, qx_hist, qy_hist, qz_hist;

int stationary_frames = 0;

int battery = 0;
int battery_hist_osc = -1;

float calc_lp_alpha(float cutoff_freq, float sample_rate) {
  float rc = 1.0f / (2.0f * PI * cutoff_freq);
  float dt = 1.0f / sample_rate;
  return dt / (rc + dt);
}

int calc_buffer_size(int sr){
  int size = (float)RMS_WINDOW / 1000.0f * sr;
  size = (size > 0) ? size : 1;
  return (float)RMS_WINDOW / 1000.0f * sr;
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

void print_data(){
    Serial.print(">> ID: ");
    Serial.print(ID);
    Serial.print(", RMS samples: ");
    Serial.print(rms_samps);
    Serial.print(", Sleep Timer: ");
    float timer = (float)stationary_frames / SAMPLERATE - RMS_WINDOW / 1000.0;
    timer = (timer > 0.0) ? SLEEP_TIMER - timer : SLEEP_TIMER;
    Serial.print(timer);
    Serial.print(", Motion: ");
    Serial.print(motion_scalar);           // Print acceleration strength to Serial Monitor
    Serial.print(", Rotation: "); 
    Serial.print(qw); Serial.print(" "); 
    Serial.print(qx); Serial.print(" "); 
    Serial.print(qy); Serial.print(" "); 
    Serial.println(qz);
}

void getHost(){
  host = MDNS.queryHost("abmac").toString();
  if(host == "0.0.0.0"){
    Serial.println(">> Could not connect to host");
    myCodeCell.SleepTimer(5);
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

  //wait for some initial motion on startup before trying for WiFi
  
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

  //attempt to connect to WiFi. Sleep if stalled for too long.
  WiFi.setSleep(false); // Added these two lines in attempt to fix a longstanding bug. 
  Wire.setTimeOut(5000); // Will hopefully prevent hangups from WiFi causing I2C timing errors.
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

  //device will be discoverable as 'abmotionID' on WiFi
  if (!MDNS.begin("abmotion" + String(ID))) {
    Serial.println(">> Error starting mDNS");
    return;
  }
  Serial.println(">> mDNS responder started");

  getHost();
  Serial.println(">> Host: " + host);

  Udp.begin(9000); 
}

void loop() {
  if (myCodeCell.Run(SAMPLERATE)) {         
    motion_scalar_hist = motion_scalar;
    motion_scalar = calc_motion_scalar();

    //Sleep after Ns of no motion
    if(motion_scalar == 0 || motion_scalar == motion_scalar_hist){
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

    if(osc_timer >= SAMPLERATE / OSC_RATE){

      battery = myCodeCell.BatteryLevelRead();
      myCodeCell.Motion_RotationVectorRead(qw, qx, qy, qz);

      if(OSC_ENABLE){
        OSCBundle bndl;

        //send motion scalar
        if(motion_scalar != motion_scalar_hist_osc){
          bndl.add("/m").add(motion_scalar);
        }

        if(qw != qw_hist || qx != qx_hist || qy != qy_hist || qz != qz_hist){
          bndl.add("/r").add(qw).add(qx).add(qy).add(qz);
        }

        //battery OSC message
        if(battery != battery_hist_osc){
          bndl.add("/b").add((int32_t)battery);
        }
        
        motion_scalar_hist_osc = motion_scalar;
        battery_hist_osc = battery;
        qw_hist = qw;
        qx_hist = qx;
        qy_hist = qy;
        qz_hist = qz;

        //send packet if it has messages
        if(bndl.size() > 0){
          Udp.beginPacket(host.c_str(), destPort);
          bndl.send(Udp);
          Udp.endPacket();
        }
      }

      print_data();

      osc_timer = 0;
    }
    else{
      osc_timer++;
    }
  }
}