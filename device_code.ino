/*
 * =============================================================================
 * Smart Alarm - ESP32-CAM (AI-Thinker)
 * =============================================================================
 *
 * ## What this firmware does
 * This sketch turns an ESP32-CAM module into a "smart alarm" device that:
 * - **Connects to WiFi** and talks to a backend server over HTTP.
 * - **Polls** the server frequently to learn whether the alarm should be ringing.
 * - When ringing, **plays a buzzer tone pattern** on a GPIO pin.
 * - When ringing, **captures camera frames** and sends them to the server.
 *   The server can respond with a "STOP" signal to stop the alarm.
 * - Runs a small **local web server** on port 80 to view the live camera stream
 *   and show the alarm status.
 *
 * ## High-level data flow
 * 1) `loop()` calls `pollServer()` every `POLL_INTERVAL` milliseconds.
 * 2) If the server indicates "ALARM_RINGING", we set `alarmRinging = true`.
 * 3) While `alarmRinging` is true:
 *    - `playAlarmTone()` runs (blocking) to generate the audible alarm.
 *    - `sendScanToServer()` sends periodic JPEG frames to the backend.
 *    - If the backend returns a payload containing "STOP", we call `stopAlarm()`.
 * 4) Independently, the local HTTP server serves:
 *    - `/` a simple HTML page
 *    - `/stream` an MJPEG stream
 *    - `/status` a JSON status endpoint
 *
 * ## Notes / constraints
 * - This code is designed for the **AI-Thinker ESP32-CAM pinout**.
 * - The alarm tone generation uses `tone()`/`noTone()`. On ESP32, this relies on
 *   LEDC timers; availability can vary with core versions.
 * - Several operations are **blocking** (HTTP requests, tone pattern delays,
 *   MJPEG stream handler loop). In practice this can:
 *   - reduce responsiveness of other tasks
 *   - cause the main loop to "stall" during long network operations
 * - Camera frames are acquired using `esp_camera_fb_get()` which returns a frame
 *   buffer that MUST be returned using `esp_camera_fb_return(fb)` to avoid leaks.
 *
 * ## Security reminder
 * WiFi credentials and server URLs are hard-coded for convenience. For production
 * use, prefer provisioning (NVS/EEPROM), WiFiManager, or secure storage patterns.
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

// =============================================================================
// CONFIGURATION
// =============================================================================
/**
 * WiFi credentials and backend server URL.
 *
 * - `WIFI_SSID` / `WIFI_PASSWORD`: Network used by the ESP32-CAM.
 * - `SERVER_URL`: Base URL of your backend service (no trailing slash required).
 *
 * The sketch uses:
 * - `GET  {SERVER_URL}/api/device/poll` to read alarm state
 * - `POST {SERVER_URL}/api/device/scan` to send JPEG frames for "dismiss" logic
 */
const char* WIFI_SSID = "Nathan's Phone";
const char* WIFI_PASSWORD = "971412811";
const char* SERVER_URL = "http://10.81.3.64:8000";

/**
 * Buzzer output pin.
 *
 * Wired to a piezo buzzer / small speaker module. This sketch uses `tone()` to
 * generate audible frequencies on this GPIO.
 */
#define AUDIO_PIN 12

/**
 * Camera pins for the AI-Thinker ESP32-CAM module.
 *
 * IMPORTANT:
 * - These values are board-specific. If you are not using AI-Thinker, you must
 *   update these pin definitions accordingly.
 * - `PWDN_GPIO_NUM` controls camera power-down (active HIGH on many modules).
 */
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

// =============================================================================
// STATE
// =============================================================================
/**
 * Whether the alarm is currently ringing.
 *
 * This is the central state variable that controls:
 * - whether we play the alarm tone pattern
 * - whether we send camera frames to the server
 * - what `/status` returns
 */
bool alarmRinging = false;

/**
 * Timing state (in milliseconds from `millis()`).
 *
 * - `lastPollTime`: last time we polled `/api/device/poll`
 * - `lastScanTime`: last time we sent a frame to `/api/device/scan`
 *
 * Intervals:
 * - `POLL_INTERVAL`: how often to poll the server regardless of ringing state
 * - `SCAN_INTERVAL`: how often to send frames while ringing
 */
unsigned long lastPollTime = 0;
unsigned long lastScanTime = 0;
const unsigned long POLL_INTERVAL = 2000;
const unsigned long SCAN_INTERVAL = 800;

/**
 * Handle for the ESP-IDF HTTP server instance.
 *
 * This sketch uses the low-level `esp_http_server` (not `WebServer`).
 */
httpd_handle_t stream_httpd = NULL;

/**
 * MJPEG stream formatting constants.
 *
 * The `/stream` endpoint responds with content type
 * `multipart/x-mixed-replace; boundary=...` and then continuously sends JPEG
 * chunks separated by a boundary marker.
 */
#define PART_BOUNDARY "123456789000000000000987654321"
static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* _STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char* _STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

// =============================================================================
// ALARM FUNCTIONS
// =============================================================================
/**
 * Play the alarm tone pattern once.
 *
 * Behavior:
 * - Plays two tones (1000Hz then 1500Hz), each for ~200ms, repeated 3 times.
 *
 * Notes:
 * - This function is **blocking** due to `delay()` calls. While it runs, the main
 *   loop does not poll the server or send frames.
 * - The main `loop()` calls this repeatedly while `alarmRinging` is true, which
 *   creates a continuous alarm sound pattern.
 */
void playAlarmTone() {
  for(int i = 0; i < 3; i++) {
    tone(AUDIO_PIN, 1000, 200);
    delay(200);
    tone(AUDIO_PIN, 1500, 200);
    delay(200);
  }
}

/**
 * Stop the alarm immediately.
 *
 * Side effects:
 * - Stops tone output (`noTone()`) and drives the audio pin LOW.
 * - Sets `alarmRinging` to false.
 * - Prints a message to Serial for debugging.
 */
void stopAlarm() {
  noTone(AUDIO_PIN);
  digitalWrite(AUDIO_PIN, LOW);
  alarmRinging = false;
  Serial.println(">>> ALARM STOPPED <<<");
}

// =============================================================================
// SERVER COMMUNICATION
// =============================================================================
/**
 * Poll the backend to determine whether the alarm should currently be ringing.
 *
 * Request:
 * - `GET {SERVER_URL}/api/device/poll`
 *
 * Expected response (simple string matching):
 * - If the response body contains `"ALARM_RINGING"` => alarm should ring.
 * - Otherwise => alarm should be off.
 *
 * State transitions:
 * - Off -> On: sets `alarmRinging = true` and logs "ALARM STARTED".
 * - On -> Off: calls `stopAlarm()`.
 *
 * Failure behavior:
 * - If the HTTP request fails or returns a non-200 status, logs the error code.
 *
 * Notes:
 * - This uses plaintext HTTP and no authentication; suitable for trusted LAN.
 * - `HTTPClient` is used in blocking mode; timeouts are set to 3 seconds.
 */
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

/**
 * Capture a camera frame and POST it to the backend for "dismiss" logic.
 *
 * Request:
 * - `POST {SERVER_URL}/api/device/scan`
 * - Content-Type: `image/jpeg`
 * - Body: raw JPEG bytes from the camera frame buffer
 *
 * Expected response (simple string matching):
 * - If the response body contains `"STOP"` => call `stopAlarm()`.
 *
 * Camera frame buffer ownership:
 * - `esp_camera_fb_get()` returns a pointer to a frame buffer owned by the driver.
 * - You MUST call `esp_camera_fb_return(fb)` exactly once after you're done, or
 *   you will leak buffers and eventually the camera capture will fail.
 *
 * Notes:
 * - This uses a 5 second HTTP timeout.
 * - It prints frame size and scan result to Serial for diagnostics.
 */
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

// =============================================================================
// WEB SERVER (for viewing camera)
// =============================================================================
/**
 * MJPEG streaming handler: `GET /stream`
 *
 * This endpoint keeps the TCP connection open and sends an infinite sequence of
 * JPEG frames, separated by `PART_BOUNDARY`.
 *
 * Important:
 * - This handler runs a `while(true)` loop and is **long-lived**. Each client
 *   streaming session consumes resources.
 * - If the client disconnects, `httpd_resp_send_chunk()` will fail and we break.
 */
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

/**
 * Simple HTML UI handler: `GET /`
 *
 * Serves a minimal page that:
 * - displays the MJPEG stream from `/stream`
 * - polls `/status` every second and displays `RINGING` or `Idle`
 */
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

/**
 * Status endpoint: `GET /status`
 *
 * Response:
 * - JSON: `{ "ringing": true|false }`
 *
 * This is used by the index page JavaScript to update the UI.
 */
static esp_err_t status_handler(httpd_req_t *req) {
  char buf[50];
  sprintf(buf, "{\"ringing\":%s}", alarmRinging ? "true" : "false");
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, buf, strlen(buf));
}

/**
 * Start the local HTTP server and register routes.
 *
 * Routes:
 * - `/`      -> `index_handler`
 * - `/stream`-> `stream_handler`
 * - `/status`-> `status_handler`
 *
 * Notes:
 * - Uses `HTTPD_DEFAULT_CONFIG()` and binds to port 80.
 * - On success, prints a message to Serial.
 */
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

// =============================================================================
// SETUP
// =============================================================================
/**
 * Arduino setup function (runs once at boot).
 *
 * Responsibilities:
 * - Disables brownout detector (common workaround on ESP32-CAM boards)
 * - Initializes Serial logging
 * - Configures the buzzer GPIO
 * - Powers on and configures the camera (resolution depends on PSRAM)
 * - Connects to WiFi and prints the assigned IP address
 * - Starts the local web server
 * - Plays a short test beep to confirm audio output
 */
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
  //
  // Key options:
  // - `pixel_format = PIXFORMAT_JPEG`: capture JPEG directly (fast, compact).
  // - `grab_mode = CAMERA_GRAB_LATEST`: discard old frames; always return latest.
  // - If PSRAM is available, use larger frames and double buffering.
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

  // Initialize camera driver. On failure, reboot after a brief delay.
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

// =============================================================================
// LOOP
// =============================================================================
/**
 * Arduino main loop (runs forever).
 *
 * Scheduling model:
 * - Uses `millis()` timestamps to run periodic tasks (server poll, scan upload).
 * - A small `delay(10)` at the end yields a little time to background work.
 *
 * Task details:
 * - Poll: every `POLL_INTERVAL` ms (2s) call `pollServer()`.
 * - If ringing:
 *   - Play alarm sound pattern (blocking).
 *   - Every `SCAN_INTERVAL` ms (0.8s) call `sendScanToServer()`.
 *
 * Caveat:
 * - Because `playAlarmTone()` blocks for ~1200ms per call, the effective scan
 *   period may be longer than `SCAN_INTERVAL` under load.
 */
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