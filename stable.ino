#include "esp_camera.h"
#include <WiFi.h>
#include "esp_timer.h"
#include "img_converters.h"
#include "Arduino.h"
#include "fb_gfx.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "esp_http_server.h"
#include <time.h>

// ===================
// WiFi credentials
// ===================
const char* ssid = "Nathan's Phone";
const char* password = "971412811";

// ===================
// ALARM SETTINGS - Change these to set your alarm time
// ===================
const int ALARM_HOUR = 13;      // 0-23 (24-hour format)
const int ALARM_MINUTE = 53;   // 0-59
const int ALARM_DURATION = 60; // How long alarm plays (seconds)

// Audio pin
#define AUDIO_PIN 12

// Timezone offset (in seconds) - Change based on your location
// For example: GMT+5 = 5*3600
const long gmtOffset_sec = 5 * 3600;  // Change this to your timezone
const int daylightOffset_sec = 0;

// Alarm state
bool alarmTriggered = false;
unsigned long alarmStartTime = 0;

// Camera pins for AI-Thinker ESP32-CAM
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

#define PART_BOUNDARY "123456789000000000000987654321"
static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* _STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* _STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

httpd_handle_t stream_httpd = NULL;

// HTML page
static const char PROGMEM INDEX_HTML[] = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <title>ESP32-CAM Alarm</title>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <style>
    body { font-family: Arial; text-align: center; margin:0px auto; padding-top: 30px; background: #f0f0f0;}
    .container { max-width: 800px; margin: 0 auto; background: white; padding: 20px; border-radius: 10px; box-shadow: 0 2px 10px rgba(0,0,0,0.1);}
    h1 { color: #333; }
    img { width: auto; max-width: 100%; height: auto; border-radius: 5px; margin: 20px 0; }
    .info { background: #e3f2fd; padding: 15px; border-radius: 5px; margin: 20px 0; }
    .alarm-info { font-size: 18px; margin: 10px 0; }
    .status { font-weight: bold; color: #1976d2; }
  </style>
</head>
<body>
  <div class="container">
    <h1>ESP32-CAM Alarm Clock</h1>
    <div class="info">
      <div class="alarm-info">Alarm set for: <span class="status" id="alarmTime">Loading...</span></div>
      <div class="alarm-info">Current time: <span class="status" id="currentTime">Loading...</span></div>
      <div class="alarm-info">Status: <span class="status" id="status">Ready</span></div>
    </div>
    <img src="/stream" id="stream">
  </div>
  <script>
    function updateTime() {
      fetch('/time')
        .then(r => r.json())
        .then(data => {
          document.getElementById('currentTime').textContent = data.current;
          document.getElementById('alarmTime').textContent = data.alarm;
          document.getElementById('status').textContent = data.status;
        });
    }
    updateTime();
    setInterval(updateTime, 1000);
  </script>
</body>
</html>
)rawliteral";

// Play alarm tone
void playAlarmTone() {
  // Play alternating tones (like a classic alarm clock)
  for(int i = 0; i < 3; i++) {
    tone(AUDIO_PIN, 1000, 200); // 1kHz for 200ms
    delay(200);
    tone(AUDIO_PIN, 1500, 200); // 1.5kHz for 200ms
    delay(200);
  }
}

// Check if it's time for alarm
void checkAlarm() {
  struct tm timeinfo;
  if(!getLocalTime(&timeinfo)){
    return;
  }
  
  int currentHour = timeinfo.tm_hour;
  int currentMinute = timeinfo.tm_min;
  int currentSecond = timeinfo.tm_sec;
  
  // Trigger alarm if time matches and not already triggered
  if(currentHour == ALARM_HOUR && currentMinute == ALARM_MINUTE && currentSecond == 0 && !alarmTriggered) {
    alarmTriggered = true;
    alarmStartTime = millis();
    Serial.println("ALARM TRIGGERED!");
  }
  
  // Play alarm sound while active
  if(alarmTriggered) {
    unsigned long elapsed = (millis() - alarmStartTime) / 1000;
    if(elapsed < ALARM_DURATION) {
      playAlarmTone();
    } else {
      alarmTriggered = false;
      Serial.println("Alarm stopped");
    }
  }
}

static esp_err_t stream_handler(httpd_req_t *req){
  camera_fb_t * fb = NULL;
  esp_err_t res = ESP_OK;
  size_t _jpg_buf_len = 0;
  uint8_t * _jpg_buf = NULL;
  char * part_buf[64];

  res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
  if(res != ESP_OK){
    return res;
  }

  while(true){
    fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("Camera capture failed");
      res = ESP_FAIL;
    } else {
      if(fb->width > 400){
        if(fb->format != PIXFORMAT_JPEG){
          bool jpeg_converted = frame2jpg(fb, 80, &_jpg_buf, &_jpg_buf_len);
          esp_camera_fb_return(fb);
          fb = NULL;
          if(!jpeg_converted){
            Serial.println("JPEG compression failed");
            res = ESP_FAIL;
          }
        } else {
          _jpg_buf_len = fb->len;
          _jpg_buf = fb->buf;
        }
      }
    }
    if(res == ESP_OK){
      size_t hlen = snprintf((char *)part_buf, 64, _STREAM_PART, _jpg_buf_len);
      res = httpd_resp_send_chunk(req, (const char *)part_buf, hlen);
    }
    if(res == ESP_OK){
      res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);
    }
    if(res == ESP_OK){
      res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
    }
    if(fb){
      esp_camera_fb_return(fb);
      fb = NULL;
      _jpg_buf = NULL;
    } else if(_jpg_buf){
      free(_jpg_buf);
      _jpg_buf = NULL;
    }
    if(res != ESP_OK){
      break;
    }
  }
  return res;
}

static esp_err_t index_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, (const char *)INDEX_HTML, strlen(INDEX_HTML));
}

static esp_err_t time_handler(httpd_req_t *req) {
  struct tm timeinfo;
  char response[200];
  
  if(!getLocalTime(&timeinfo)){
    sprintf(response, "{\"current\":\"Time not set\",\"alarm\":\"%02d:%02d\",\"status\":\"Waiting\"}", 
            ALARM_HOUR, ALARM_MINUTE);
  } else {
    char timeStr[20];
    strftime(timeStr, sizeof(timeStr), "%H:%M:%S", &timeinfo);
    
    const char* status = alarmTriggered ? "ALARM RINGING!" : "Ready";
    
    sprintf(response, "{\"current\":\"%s\",\"alarm\":\"%02d:%02d\",\"status\":\"%s\"}", 
            timeStr, ALARM_HOUR, ALARM_MINUTE, status);
  }
  
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, response, strlen(response));
}

void startCameraServer(){
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;

  httpd_uri_t index_uri = {
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = index_handler,
    .user_ctx  = NULL
  };

  httpd_uri_t stream_uri = {
    .uri       = "/stream",
    .method    = HTTP_GET,
    .handler   = stream_handler,
    .user_ctx  = NULL
  };
  
  httpd_uri_t time_uri = {
    .uri       = "/time",
    .method    = HTTP_GET,
    .handler   = time_handler,
    .user_ctx  = NULL
  };
  
  if (httpd_start(&stream_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(stream_httpd, &index_uri);
    httpd_register_uri_handler(stream_httpd, &stream_uri);
    httpd_register_uri_handler(stream_httpd, &time_uri);
  }s
}

void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
  
  Serial.begin(9600);
  Serial.setDebugOutput(false);
  delay(1000);
  Serial.println();
  Serial.println("ESP32-CAM Alarm Clock Starting...");

  // Setup audio pin
  pinMode(AUDIO_PIN, OUTPUT);
  digitalWrite(AUDIO_PIN, LOW);

  // Power up camera
  pinMode(32, OUTPUT);
  digitalWrite(32, LOW);
  delay(100);
  
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  config.grab_mode = CAMERA_GRAB_LATEST;
  
  if(psramFound()){
    config.frame_size = FRAMESIZE_SVGA;
    config.jpeg_quality = 12;
    config.fb_count = 2;
    config.fb_location = CAMERA_FB_IN_PSRAM;
    Serial.println("PSRAM found");
  } else {
    config.frame_size = FRAMESIZE_SVGA;
    config.jpeg_quality = 12;
    config.fb_count = 1;
    config.fb_location = CAMERA_FB_IN_DRAM;
    Serial.println("PSRAM not found");
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x\n", err);
    Serial.println("Restarting in 3 seconds...");
    delay(3000);
    ESP.restart();
    return;
  }
  
  Serial.println("Camera initialized successfully!");

  sensor_t * s = esp_camera_sensor_get();
  s->set_framesize(s, FRAMESIZE_VGA);

  // Connect to WiFi
  WiFi.begin(ssid, password);
  WiFi.setSleep(false);

  Serial.print("Connecting to WiFi");
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println("");
  Serial.println("WiFi connected!");

  // Initialize time from NTP server
  configTime(gmtOffset_sec, daylightOffset_sec, "pool.ntp.org", "time.nist.gov");
  Serial.println("Waiting for time sync...");
  delay(2000);
  
  struct tm timeinfo;
  if(getLocalTime(&timeinfo)){
    Serial.println("Time synchronized!");
    Serial.printf("Current time: %02d:%02d:%02d\n", timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
  }

  startCameraServer();

  Serial.print("Camera Ready! Use 'http://");
  Serial.print(WiFi.localIP());
  Serial.println("' to connect");
  Serial.printf("Alarm set for: %02d:%02d\n", ALARM_HOUR, ALARM_MINUTE);
  
  // Test beep
  tone(AUDIO_PIN, 1000, 500);
  delay(600);
  Serial.println("Audio test complete");
}

void loop() {
  checkAlarm();
  delay(100);
}