/*
 * Smart Alarm - ESP32-CAM
 * Based on user's stable original code
 * Camera always on, server polling added
 */

#include "esp_camera.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include "esp_timer.h"
#include "img_converters.h"
#include "Arduino.h"
#include "fb_gfx.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include "esp_http_server.h"

// ===================
// CONFIGURATION
// ===================
const char* WIFI_SSID = "Nathan's Phone";
const char* WIFI_PASSWORD = "971412811";
const char* SERVER_URL = "http://10.81.3.64:8000";

// Audio pin
#define AUDIO_PIN 12

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

// ===================
// STATE
// ===================
bool alarmRinging = false;
unsigned long lastPollTime = 0;
unsigned long lastScanTime = 0;
const unsigned long POLL_INTERVAL = 2000;
const unsigned long SCAN_INTERVAL = 800;

httpd_handle_t stream_httpd = NULL;

#define PART_BOUNDARY "123456789000000000000987654321"
static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* _STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* _STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

// ===================
// ALARM FUNCTIONS
// ===================
void playAlarmTone() {
  for(int i = 0; i < 3; i++) {
    tone(AUDIO_PIN, 1000, 200);
    delay(200);
    tone(AUDIO_PIN, 1500, 200);
    delay(200);
  }
}

void stopAlarm() {
  noTone(AUDIO_PIN);
  digitalWrite(AUDIO_PIN, LOW);
  alarmRinging = false;
  Serial.println(">>> ALARM STOPPED <<<");
}

// ===================
// SERVER COMMUNICATION
// ===================
void pollServer() {
  HTTPClient http;
  String url = String(SERVER_URL) + "/api/device/poll";
  
  http.begin(url);
  http.setTimeout(3000);
  
  int httpCode = http.GET();
  
  if (httpCode == 200) {
    String response = http.getString();
    
    bool shouldRing = response.indexOf("ALARM_RINGING") >= 0;
    
    if (shouldRing && !alarmRinging) {
      alarmRinging = true;
      Serial.println(">>> ALARM STARTED <<<");
    } else if (!shouldRing && alarmRinging) {
      stopAlarm();
    }
  } else {
    Serial.printf("Poll error: %d\n", httpCode);
  }
  
  http.end();
}

void sendScanToServer() {
  camera_fb_t* fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Capture failed");
    return;
  }
  
  Serial.printf("Sending frame: %d bytes\n", fb->len);
  
  HTTPClient http;
  String url = String(SERVER_URL) + "/api/device/scan";
  
  http.begin(url);
  http.setTimeout(5000);
  http.addHeader("Content-Type", "image/jpeg");
  
  int httpCode = http.POST(fb->buf, fb->len);
  esp_camera_fb_return(fb);
  
  if (httpCode == 200) {
    String response = http.getString();
    Serial.printf("Scan result: %s\n", response.c_str());
    
    if (response.indexOf("STOP") >= 0) {
      stopAlarm();
    }
  }
  
  http.end();
}

// ===================
// WEB SERVER (for viewing camera)
// ===================
static esp_err_t stream_handler(httpd_req_t *req){
  camera_fb_t * fb = NULL;
  esp_err_t res = ESP_OK;
  size_t _jpg_buf_len = 0;
  uint8_t * _jpg_buf = NULL;
  char * part_buf[64];

  res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
  if(res != ESP_OK) return res;

  while(true){
    fb = esp_camera_fb_get();
    if (!fb) {
      res = ESP_FAIL;
    } else {
      _jpg_buf_len = fb->len;
      _jpg_buf = fb->buf;
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
    }
    if(res != ESP_OK) break;
  }
  return res;
}

static esp_err_t index_handler(httpd_req_t *req) {
  const char* html = "<html><body style='text-align:center'>"
    "<h1>Smart Alarm Camera</h1>"
    "<p>Status: <b id='s'>Loading...</b></p>"
    "<img src='/stream' style='max-width:100%'>"
    "<script>setInterval(()=>fetch('/status').then(r=>r.json()).then(d=>document.getElementById('s').innerText=d.ringing?'RINGING':'Idle'),1000)</script>"
    "</body></html>";
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, html, strlen(html));
}

static esp_err_t status_handler(httpd_req_t *req) {
  char buf[50];
  sprintf(buf, "{\"ringing\":%s}", alarmRinging ? "true" : "false");
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, buf, strlen(buf));
}

void startWebServer(){
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;

  httpd_uri_t index_uri = { .uri = "/", .method = HTTP_GET, .handler = index_handler };
  httpd_uri_t stream_uri = { .uri = "/stream", .method = HTTP_GET, .handler = stream_handler };
  httpd_uri_t status_uri = { .uri = "/status", .method = HTTP_GET, .handler = status_handler };
  
  if (httpd_start(&stream_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(stream_httpd, &index_uri);
    httpd_register_uri_handler(stream_httpd, &stream_uri);
    httpd_register_uri_handler(stream_httpd, &status_uri);
    Serial.println("Web server started on port 80");
  }
}

// ===================
// SETUP
// ===================
void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
  
  Serial.begin(9600);
  delay(1000);
  Serial.println("\n================================");
  Serial.println("Smart Alarm - ESP32-CAM");
  Serial.println("================================");

  // Audio pin
  pinMode(AUDIO_PIN, OUTPUT);
  digitalWrite(AUDIO_PIN, LOW);

  // Camera power
  pinMode(PWDN_GPIO_NUM, OUTPUT);
  digitalWrite(PWDN_GPIO_NUM, LOW);
  delay(100);
  
  // Camera config
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
    config.frame_size = FRAMESIZE_VGA;
    config.jpeg_quality = 12;
    config.fb_count = 2;
    config.fb_location = CAMERA_FB_IN_PSRAM;
    Serial.println("PSRAM: Yes");
  } else {
    config.frame_size = FRAMESIZE_CIF;
    config.jpeg_quality = 15;
    config.fb_count = 1;
    config.fb_location = CAMERA_FB_IN_DRAM;
    Serial.println("PSRAM: No");
  }

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed: 0x%x\n", err);
    delay(3000);
    ESP.restart();
  }
  Serial.println("Camera: OK");

  // WiFi
  Serial.print("WiFi: Connecting");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  WiFi.setSleep(false);
  
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  Serial.println(" Connected!");
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());

  // Start web server
  startWebServer();
  
  // Test beep
  Serial.println("Testing buzzer...");
  tone(AUDIO_PIN, 1000, 500);
  delay(600);
  
  Serial.println("================================");
  Serial.print("Server: ");
  Serial.println(SERVER_URL);
  Serial.println("Ready!");
  Serial.println("================================");
}

// ===================
// LOOP
// ===================
void loop() {
  unsigned long now = millis();
  
  // Poll server every 2 seconds
  if (now - lastPollTime >= POLL_INTERVAL) {
    pollServer();
    lastPollTime = now;
  }
  
  // If ringing: play sound and send frames
  if (alarmRinging) {
    playAlarmTone();
    
    if (now - lastScanTime >= SCAN_INTERVAL) {
      sendScanToServer();
      lastScanTime = now;
    }
  }
  
  delay(10);
}