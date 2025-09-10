//Kode Complete dengan Google Sheets Integration - Modified MQTT Data
#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>

// ========== KONFIGURASI JARINGAN ==========
// WiFi Lokal (Router tanpa internet)
const char* local_ssid = "WirelessNet";
const char* local_password = "eeeeeeee";

// Hotspot Internet (untuk MQTT dan Google Sheets)
const char* internet_ssid = "Boss-Kaled";  // Ganti dengan nama hotspot Anda
const char* internet_password = "www.kipli";   // Ganti dengan password hotspot

// Konfigurasi Static IP untuk jaringan lokal
IPAddress local_IP(192, 168, 100, 3);
IPAddress gateway(192, 168, 100, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress primaryDNS(8, 8, 8, 8);
IPAddress secondaryDNS(8, 8, 4, 4);

// ========== KONFIGURASI MQTT ==========
const char* mqtt_server = "broker.hivemq.com";
const int mqtt_port = 1883;
const char* mqtt_client_id = "ESP32_VehicleCounter_001";  // Buat unique ID
const char* mqtt_topic_base = "vehicle_counter/esp32_001"; // Base topic

// ========== KONFIGURASI GOOGLE SHEETS ==========
// Ganti dengan URL Google Apps Script Web App Anda
const char* google_script_url = "https://script.google.com/macros/s/AKfycbydT-zW8wSihyvPfHOa319eRT5LBbWtFBg4B00zKQCdFkA9nRTeoTudQDUCYs_yPZ_OXA/exec";

// ========== VARIABEL SISTEM ==========
WebServer server(80);
WiFiClient espClient;
PubSubClient mqtt_client(espClient);
LiquidCrystal_I2C lcd(0x27, 16, 2);

// Network management
enum NetworkMode {
  LOCAL_NETWORK,    // Terhubung ke router lokal
  INTERNET_NETWORK  // Terhubung ke hotspot internet
};

NetworkMode current_network = LOCAL_NETWORK;
unsigned long last_network_switch = 0;
unsigned long network_switch_interval = 15000;  // Switch setiap 30 detik
unsigned long local_duration = 10000;           // 10 detik di lokal
unsigned long internet_duration = 5000;         // 5 detik di internet

// Data variables
int countUp = 0;
int countDown = 0;
String lastTimestamp = "";
bool lcdConnected = false;
unsigned long lastUpdateTime = 0;

// Traffic status
String trafficStatus = "SEPI";
unsigned long statusUpdateInterval = 60000;
unsigned long lastStatusUpdate = 0;
int previousTotal = 0;

// MODIFIED MQTT data buffer - now matches Google Sheets data
struct MQTTDataBuffer {
  int count_up;
  int count_down;
  int total_vehicles;
  String traffic_status;
  String timestamp;
  String date_time;              // ADDED: Same as Google Sheets
  String device_id;              // ADDED: Same as Google Sheets  
  int wifi_rssi;
  unsigned long uptime_seconds;  // MODIFIED: Changed from uptime to uptime_seconds
  String ip_address;
  bool lcd_connected;            // KEPT: Additional info for MQTT monitoring
  bool data_pending;
};

MQTTDataBuffer mqtt_buffer = {0, 0, 0, "SEPI", "", "", "", 0, 0, "", false, false};

// Google Sheets data buffer (unchanged)
struct GoogleSheetsBuffer {
  bool data_pending;
  int count_up;
  int count_down;
  int total_vehicles;
  String traffic_status;
  String timestamp;
  String date_time;
  int wifi_rssi;
  unsigned long uptime;
  unsigned long last_upload_attempt;
  int upload_attempts;
  bool upload_success;
};

GoogleSheetsBuffer sheets_buffer = {false, 0, 0, 0, "SEPI", "", "", 0, 0, 0, 0, false};

// Upload intervals
unsigned long sheets_upload_interval = 300000;  // Upload ke Google Sheets setiap 5 menit
unsigned long last_sheets_upload = 0;

// LED pins
const int ledUpPin = 2;
const int ledDownPin = 4;
const int ledBuiltIn = 2;

void setup() {
  Serial.begin(115200);
  delay(1000);
  
  Serial.println("\n========== ESP32 Vehicle Counter with MQTT & Google Sheets ==========");
  
  // Setup LED pins
  pinMode(ledUpPin, OUTPUT);
  pinMode(ledDownPin, OUTPUT);
  pinMode(ledBuiltIn, OUTPUT);
  
  // LED test sequence
  ledTestSequence();
  
  // Initialize I2C and LCD
  Wire.begin(21, 22);
  testLCDConnection();
  
  // Initialize MQTT client
  mqtt_client.setServer(mqtt_server, mqtt_port);
  
  // Start with local network
  switchToLocalNetwork();
  
  Serial.println("========================================");
  
  if (lcdConnected) {
    updateLCDReady();
  }
}

void loop() {
  // Handle network time-sharing
  handleNetworkTimeSharing();
  
  // Handle berdasarkan mode jaringan
  if (current_network == LOCAL_NETWORK) {
    server.handleClient();
    
    // Update traffic status setiap menit
    if (millis() - lastStatusUpdate > statusUpdateInterval) {
      updateTrafficStatus();
    }
    
    // Check if it's time to upload to Google Sheets
    if (millis() - last_sheets_upload > sheets_upload_interval) {
      prepareDataForGoogleSheets();
    }
    
  } else if (current_network == INTERNET_NETWORK) {
    // Handle MQTT connection dan upload data
    handleMQTTCommunication();
    
    // Handle Google Sheets upload
    handleGoogleSheetsUpload();
  }
  
  // System status indicators
  handleStatusIndicators();
  
  delay(50);
}

// ========== NETWORK TIME-SHARING FUNCTIONS ==========
void handleNetworkTimeSharing() {
  unsigned long current_time = millis();
  unsigned long time_in_current_mode = current_time - last_network_switch;
  
  if (current_network == LOCAL_NETWORK && time_in_current_mode >= local_duration) {
    // Beralih ke internet untuk upload MQTT dan Google Sheets
    if (mqtt_buffer.data_pending || sheets_buffer.data_pending) {
      switchToInternetNetwork();
    }
  } else if (current_network == INTERNET_NETWORK && time_in_current_mode >= internet_duration) {
    // Kembali ke jaringan lokal
    switchToLocalNetwork();
  }
}

void switchToLocalNetwork() {
  Serial.println("\n🔄 Switching to LOCAL NETWORK...");
  
  WiFi.disconnect();
  delay(1000);
  
  // Konfigurasi static IP untuk jaringan lokal
  if (!WiFi.config(local_IP, gateway, subnet, primaryDNS, secondaryDNS)) {
    Serial.println("✗ Failed to configure Static IP");
  }
  
  WiFi.begin(local_ssid, local_password);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✓ Connected to LOCAL NETWORK");
    Serial.println("IP: " + WiFi.localIP().toString());
    
    current_network = LOCAL_NETWORK;
    last_network_switch = millis();
    
    // Start web server
    setupWebServer();
    server.begin();
    
    if (lcdConnected) {
      lcd.setCursor(0, 1);
      lcd.print("Mode: LOCAL     ");
    }
  } else {
    Serial.println("\n✗ Failed to connect to LOCAL NETWORK");
  }
}

void switchToInternetNetwork() {
  Serial.println("\n🌐 Switching to INTERNET NETWORK...");
  
  // Stop web server
  server.stop();
  
  WiFi.disconnect();
  delay(1000);
  
  // Reset to DHCP for internet connection
  WiFi.config(0U, 0U, 0U, 0U, 0U);
  
  WiFi.begin(internet_ssid, internet_password);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 20) {
    delay(500);
    Serial.print(".");
    attempts++;
  }
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n✓ Connected to INTERNET NETWORK");
    Serial.println("IP: " + WiFi.localIP().toString());
    
    current_network = INTERNET_NETWORK;
    last_network_switch = millis();
    
    if (lcdConnected) {
      lcd.setCursor(0, 1);
      lcd.print("Mode: INTERNET  ");
    }
  } else {
    Serial.println("\n✗ Failed to connect to INTERNET NETWORK");
    // Fallback to local network
    switchToLocalNetwork();
  }
}

// ========== GOOGLE SHEETS FUNCTIONS ==========
void prepareDataForGoogleSheets() {
  sheets_buffer.data_pending = true;
  sheets_buffer.count_up = countUp;
  sheets_buffer.count_down = countDown;
  sheets_buffer.total_vehicles = countUp + countDown;
  sheets_buffer.traffic_status = trafficStatus;
  sheets_buffer.timestamp = lastTimestamp;
  sheets_buffer.date_time = getCurrentDateTime();
  sheets_buffer.wifi_rssi = WiFi.RSSI();
  sheets_buffer.uptime = millis();
  sheets_buffer.upload_attempts = 0;
  sheets_buffer.upload_success = false;
  
  last_sheets_upload = millis();
  
  Serial.println("📊 Data prepared for Google Sheets upload");
}

void handleGoogleSheetsUpload() {
  if (!sheets_buffer.data_pending) return;
  if (sheets_buffer.upload_attempts >= 3) return; // Max 3 attempts
  
  // Wait at least 5 seconds between attempts
  if (millis() - sheets_buffer.last_upload_attempt < 5000) return;
  
  uploadDataToGoogleSheets();
}

void uploadDataToGoogleSheets() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("❌ WiFi not connected for Google Sheets upload");
    return;
  }
  
  Serial.println("📊 Uploading data to Google Sheets...");
  sheets_buffer.upload_attempts++;
  sheets_buffer.last_upload_attempt = millis();
  
  HTTPClient http;
  http.begin(google_script_url);
  http.addHeader("Content-Type", "application/json");
  
  // Create JSON payload for Google Sheets
  StaticJsonDocument<512> doc;
  doc["count_up"] = sheets_buffer.count_up;
  doc["count_down"] = sheets_buffer.count_down;
  doc["total_vehicles"] = sheets_buffer.total_vehicles;
  doc["traffic_status"] = sheets_buffer.traffic_status;
  doc["timestamp"] = sheets_buffer.timestamp;
  doc["date_time"] = sheets_buffer.date_time;
  doc["device_id"] = mqtt_client_id;
  doc["wifi_rssi"] = sheets_buffer.wifi_rssi;
  doc["uptime_seconds"] = sheets_buffer.uptime / 1000;
  doc["ip_address"] = WiFi.localIP().toString();
  
  String json_string;
  serializeJson(doc, json_string);
  
  Serial.println("Sending JSON: " + json_string);
  
  int httpResponseCode = http.POST(json_string);
  
  if (httpResponseCode > 0) {
    String response = http.getString();
    Serial.println("Google Sheets Response Code: " + String(httpResponseCode));
    Serial.println("Response: " + response);
    
    if (httpResponseCode == 200) {
      Serial.println("✅ Data successfully uploaded to Google Sheets");
      sheets_buffer.data_pending = false;
      sheets_buffer.upload_success = true;
      
      // Blink success indicator
      for (int i = 0; i < 3; i++) {
        digitalWrite(ledBuiltIn, HIGH);
        delay(100);
        digitalWrite(ledBuiltIn, LOW);
        delay(100);
      }
    } else {
      Serial.println("❌ Google Sheets upload failed with code: " + String(httpResponseCode));
    }
  } else {
    Serial.println("❌ Google Sheets upload error: " + String(httpResponseCode));
  }
  
  http.end();
}

String getCurrentDateTime() {
  // Simple timestamp - for better timestamp, consider using NTP
  unsigned long currentTime = millis();
  unsigned long seconds = currentTime / 1000;
  unsigned long minutes = seconds / 60;
  unsigned long hours = minutes / 60;
  unsigned long days = hours / 24;
  
  // Format: Day HH:MM:SS (simple uptime format)
  String timeStr = "Day" + String(days) + " ";
  timeStr += String(hours % 24) + ":";
  timeStr += String(minutes % 60) + ":";
  timeStr += String(seconds % 60);
  
  return timeStr;
}

// ========== MODIFIED MQTT FUNCTIONS ==========
void handleMQTTCommunication() {
  if (!mqtt_client.connected()) {
    reconnectMQTT();
  }
  
  if (mqtt_client.connected()) {
    mqtt_client.loop();
    
    // Upload data jika ada yang pending
    if (mqtt_buffer.data_pending) {
      uploadDataToMQTT();
    }
  }
}

void reconnectMQTT() {
  int attempts = 0;
  while (!mqtt_client.connected() && attempts < 3) {
    Serial.print("🔗 Connecting to MQTT broker...");
    
    if (mqtt_client.connect(mqtt_client_id)) {
      Serial.println(" ✓ Connected!");
    } else {
      Serial.println(" ✗ Failed, rc=" + String(mqtt_client.state()));
      attempts++;
      delay(1000);
    }
  }
}

void uploadDataToMQTT() {
  if (!mqtt_client.connected()) return;
  
  Serial.println("📤 Uploading data to MQTT...");
  
  // Create JSON payload - NOW MATCHES Google Sheets format
  StaticJsonDocument<1024> doc;
  doc["count_up"] = mqtt_buffer.count_up;
  doc["count_down"] = mqtt_buffer.count_down;
  doc["total_vehicles"] = mqtt_buffer.total_vehicles;
  doc["traffic_status"] = mqtt_buffer.traffic_status;
  doc["timestamp"] = mqtt_buffer.timestamp;
  doc["date_time"] = mqtt_buffer.date_time;              // NEW: Added date_time
  doc["device_id"] = mqtt_buffer.device_id;              // NEW: Added device_id
  doc["wifi_rssi"] = mqtt_buffer.wifi_rssi;
  doc["uptime_seconds"] = mqtt_buffer.uptime_seconds;    // MODIFIED: Now in seconds
  doc["ip_address"] = mqtt_buffer.ip_address;
  doc["lcd_connected"] = mqtt_buffer.lcd_connected;      // KEPT: Additional MQTT info
  
  String json_string;
  serializeJson(doc, json_string);
  
  // Publish to different topics
  String base_topic = String(mqtt_topic_base);
  
  // Individual topics for easy monitoring (same as before)
  mqtt_client.publish((base_topic + "/count_up").c_str(), String(mqtt_buffer.count_up).c_str(), true);
  mqtt_client.publish((base_topic + "/count_down").c_str(), String(mqtt_buffer.count_down).c_str(), true);
  mqtt_client.publish((base_topic + "/total_vehicles").c_str(), String(mqtt_buffer.total_vehicles).c_str(), true);
  mqtt_client.publish((base_topic + "/traffic_status").c_str(), mqtt_buffer.traffic_status.c_str(), true);
  mqtt_client.publish((base_topic + "/timestamp").c_str(), mqtt_buffer.timestamp.c_str(), true);
  
  // NEW: Additional individual topics matching Google Sheets
  mqtt_client.publish((base_topic + "/date_time").c_str(), mqtt_buffer.date_time.c_str(), true);
  mqtt_client.publish((base_topic + "/device_id").c_str(), mqtt_buffer.device_id.c_str(), true);
  mqtt_client.publish((base_topic + "/uptime_seconds").c_str(), String(mqtt_buffer.uptime_seconds).c_str(), true);
  mqtt_client.publish((base_topic + "/wifi_rssi").c_str(), String(mqtt_buffer.wifi_rssi).c_str(), true);
  mqtt_client.publish((base_topic + "/ip_address").c_str(), mqtt_buffer.ip_address.c_str(), true);
  
  // Complete data in JSON format - NOW MATCHES Google Sheets
  mqtt_client.publish((base_topic + "/data").c_str(), json_string.c_str(), true);
  
  // System status (MQTT specific)
  mqtt_client.publish((base_topic + "/system/online").c_str(), "true", true);
  mqtt_client.publish((base_topic + "/system/lcd_connected").c_str(), String(mqtt_buffer.lcd_connected ? "true" : "false").c_str(), true);
  
  Serial.println("✓ Data uploaded successfully to MQTT");
  Serial.println("JSON: " + json_string);
  
  mqtt_buffer.data_pending = false;
  
  // Blink indicator
  blinkLED(ledBuiltIn, 200);
}

// MODIFIED: prepareDataForMQTT now includes all Google Sheets fields
void prepareDataForMQTT() {
  mqtt_buffer.count_up = countUp;
  mqtt_buffer.count_down = countDown;
  mqtt_buffer.total_vehicles = countUp + countDown;
  mqtt_buffer.traffic_status = trafficStatus;
  mqtt_buffer.timestamp = lastTimestamp;
  mqtt_buffer.date_time = getCurrentDateTime();          // NEW: Added date_time
  mqtt_buffer.device_id = mqtt_client_id;                // NEW: Added device_id
  mqtt_buffer.wifi_rssi = WiFi.RSSI();
  mqtt_buffer.uptime_seconds = millis() / 1000;          // MODIFIED: Now in seconds
  mqtt_buffer.ip_address = WiFi.localIP().toString();
  mqtt_buffer.lcd_connected = lcdConnected;              // KEPT: MQTT specific info
  mqtt_buffer.data_pending = true;
  
  Serial.println("📋 Data prepared for MQTT upload (matches Google Sheets format)");
}

// ========== WEB SERVER FUNCTIONS ==========
void setupWebServer() {
  server.on("/", handleRoot);
  server.on("/update", HTTP_POST, handleUpdate);
  server.on("/status", handleStatus);
  server.on("/test", handleTest);
  server.on("/reset", handleReset);
  server.on("/mqtt_status", handleMQTTStatus);
  server.on("/sheets_status", handleSheetsStatus);
  server.on("/force_sheets_upload", handleForceSheets);
}

void handleRoot() {
  String html = "<!DOCTYPE html><html><head>";
  html += "<title>ESP32 Vehicle Counter with MQTT & Google Sheets</title>";
  html += "<meta charset='UTF-8'>";
  html += "<meta http-equiv='refresh' content='3'>";
  html += "<style>";
  html += "body { font-family: Arial, sans-serif; margin: 20px; background: linear-gradient(135deg, #667eea 0%, #764ba2 100%); color: white; }";
  html += ".container { max-width: 700px; margin: 0 auto; background: rgba(255,255,255,0.1); padding: 30px; border-radius: 15px; backdrop-filter: blur(10px); }";
  html += ".status { font-size: 36px; font-weight: bold; text-align: center; padding: 20px; border-radius: 15px; margin: 20px 0; }";
  html += ".sepi { background: linear-gradient(45deg, #28a745, #20c997); }";
  html += ".lancar { background: linear-gradient(45deg, #17a2b8, #138496); }";
  html += ".ramai { background: linear-gradient(45deg, #ffc107, #e0a800); }";
  html += ".padat { background: linear-gradient(45deg, #dc3545, #c82333); }";
  html += ".counter { display: inline-block; margin: 15px; padding: 25px; border-radius: 10px; color: white; font-size: 28px; font-weight: bold; min-width: 200px; text-align: center; }";
  html += ".count-up { background: linear-gradient(45deg, #007bff, #0056b3); box-shadow: 0 4px 15px rgba(0,123,255,0.3); }";
  html += ".count-down { background: linear-gradient(45deg, #dc3545, #c82333); box-shadow: 0 4px 15px rgba(220,53,69,0.3); }";
  html += ".info { background: rgba(255,255,255,0.1); padding: 15px; border-radius: 10px; margin: 10px 0; }";
  html += ".status-ok { color: #28a745; font-weight: bold; }";
  html += ".status-error { color: #ffc107; font-weight: bold; }";
  html += ".status-success { color: #28a745; font-weight: bold; }";
  html += ".button { display: inline-block; padding: 10px 20px; margin: 5px; background: #007bff; color: white; text-decoration: none; border-radius: 5px; }";
  html += ".network-mode { background: linear-gradient(45deg, #6f42c1, #5a32a3); color: white; padding: 10px; border-radius: 10px; text-align: center; margin: 10px 0; }";
  html += ".sheets-status { background: linear-gradient(45deg, #20c997, #17a085); color: white; padding: 10px; border-radius: 10px; text-align: center; margin: 10px 0; }";
  html += ".mqtt-info { background: linear-gradient(45deg, #fd7e14, #e8590c); color: white; padding: 10px; border-radius: 10px; text-align: center; margin: 10px 0; }";
  html += "</style></head><body>";
  html += "<div class='container'>";
  html += "<h1>🚗 Vehicle Counter with MQTT & Google Sheets</h1>";
  
  // Network Mode Indicator
  String mode_text = (current_network == LOCAL_NETWORK) ? "LOCAL NETWORK" : "INTERNET NETWORK";
  html += "<div class='network-mode'>📡 Mode: " + mode_text + "</div>";
  
  // MQTT Status Info
  String mqtt_status_text = "MQTT: ";
  if (mqtt_buffer.data_pending) {
    mqtt_status_text += "Data pending upload ⏳";
  } else {
    mqtt_status_text += "Synced ✅ (Enhanced format)";
  }
  html += "<div class='mqtt-info'>" + mqtt_status_text + "</div>";
  
  // Google Sheets Status
  String sheets_status_text = "Google Sheets: ";
  if (sheets_buffer.upload_success) {
    sheets_status_text += "Last upload SUCCESS ✅";
  } else if (sheets_buffer.data_pending) {
    sheets_status_text += "Data pending upload (Attempts: " + String(sheets_buffer.upload_attempts) + "/3) ⏳";
  } else {
    sheets_status_text += "No pending data 📊";
  }
  html += "<div class='sheets-status'>" + sheets_status_text + "</div>";
  
  // Status Traffic
  String statusClass = trafficStatus;
  statusClass.toLowerCase();
  if (statusClass == "no data") statusClass = "padat";
  html += "<div class='status " + statusClass + "'>STATUS: " + trafficStatus + "</div>";
  
  html += "<div class='counter count-up'>↑ Menjauh<br>" + String(countUp) + "</div>";
  html += "<div class='counter count-down'>↓ Mendekat<br>" + String(countDown) + "</div>";
  html += "<div class='info'>🚦 Total Kendaraan: " + String(countUp + countDown) + "</div>";
  html += "<div class='info'>⏰ Last Update: " + (lastTimestamp.length() > 0 ? lastTimestamp : "No data yet") + "</div>";
  html += "<div class='info'>🕒 Date Time: " + getCurrentDateTime() + "</div>";
  html += "<div class='info'>🌐 ESP32 IP: " + WiFi.localIP().toString() + "</div>";
  html += "<div class='info'>📡 WiFi: " + String(WiFi.RSSI()) + " dBm</div>";
  html += "<div class='info'>📺 LCD: <span class='" + String(lcdConnected ? "status-ok" : "status-error") + "'>" + String(lcdConnected ? "Connected ✓" : "Disconnected ✗") + "</span></div>";
  html += "<div class='info'>📤 MQTT Buffer: <span class='" + String(mqtt_buffer.data_pending ? "status-error" : "status-ok") + "'>" + String(mqtt_buffer.data_pending ? "Data Pending" : "Synced (Enhanced)") + "</span></div>";
  html += "<div class='info'>📊 Google Sheets: <span class='" + String(sheets_buffer.data_pending ? "status-error" : "status-success") + "'>" + String(sheets_buffer.data_pending ? "Data Pending" : "Synced") + "</span></div>";
  html += "<div class='info'>⏱️ Next Sheets Upload: " + String((sheets_upload_interval - (millis() - last_sheets_upload)) / 1000) + " seconds</div>";
  html += "<div class='info'>⏱️ Uptime: " + String(millis()/1000) + " seconds</div>";
  html += "<div class='info'>";
  html += "<a href='/test' class='button'>Test LCD</a>";
  html += "<a href='/reset' class='button'>Reset Counters</a>";
  html += "<a href='/mqtt_status' class='button'>MQTT Status</a>";
  html += "<a href='/sheets_status' class='button'>Sheets Status</a>";
  html += "<a href='/force_sheets_upload' class='button'>Force Upload Sheets</a>";
  html += "</div>";
  html += "</div></body></html>";
  
  server.send(200, "text/html", html);
}

void handleUpdate() {
  if (server.hasArg("plain")) {
    String body = server.arg("plain");
    Serial.println("📨 Received data: " + body);
    
    // Manual JSON parsing
    int upIndex = body.indexOf("\"count_up\":");
    int downIndex = body.indexOf("\"count_down\":");
    int timestampIndex = body.indexOf("\"timestamp\":");
    
    if (upIndex != -1 && downIndex != -1) {
      // Extract count_up
      int upStart = body.indexOf(':', upIndex) + 1;
      int upEnd = body.indexOf(',', upStart);
      if (upEnd == -1) upEnd = body.indexOf('}', upStart);
      countUp = body.substring(upStart, upEnd).toInt();
      
      // Extract count_down
      int downStart = body.indexOf(':', downIndex) + 1;
      int downEnd = body.indexOf(',', downStart);
      if (downEnd == -1) downEnd = body.indexOf('}', downStart);
      countDown = body.substring(downStart, downEnd).toInt();
      
      // Extract timestamp
      if (timestampIndex != -1) {
        int tsStart = body.indexOf('\"', timestampIndex + 12) + 1;
        int tsEnd = body.indexOf('\"', tsStart);
        if (tsStart > 0 && tsEnd > tsStart) {
          lastTimestamp = body.substring(tsStart, tsEnd);
        }
      }
      
      lastUpdateTime = millis();
      
      Serial.println("=== Counter Update ===");
      Serial.println("Count Up: " + String(countUp));
      Serial.println("Count Down: " + String(countDown));
      Serial.println("Timestamp: " + lastTimestamp);
      Serial.println("======================");
      
      // LED indicators
      blinkLED(ledUpPin, 100);
      blinkLED(ledDownPin, 100);
      
      // Update LCD
      updateLCD();
      
      // Prepare data for MQTT upload (now with enhanced format)
      prepareDataForMQTT();
      
      server.send(200, "text/plain", "Data received successfully");
    } else {
      Serial.println("✗ Invalid JSON format");
      server.send(400, "text/plain", "Invalid JSON format");
    }
  } else {
    Serial.println("✗ No data in request body");
    server.send(400, "text/plain", "No data received");
  }
}

void handleStatus() {
  String response = "{";
  response += "\"count_up\":" + String(countUp) + ",";
  response += "\"count_down\":" + String(countDown) + ",";
  response += "\"total_vehicles\":" + String(countUp + countDown) + ",";
  response += "\"traffic_status\":\"" + trafficStatus + "\",";
  response += "\"timestamp\":\"" + lastTimestamp + "\",";
  response += "\"date_time\":\"" + getCurrentDateTime() + "\",";
  response += "\"device_id\":\"" + String(mqtt_client_id) + "\",";
  response += "\"ip_address\":\"" + WiFi.localIP().toString() + "\",";
  response += "\"lcd_connected\":" + String(lcdConnected ? "true" : "false") + ",";
  response += "\"wifi_rssi\":" + String(WiFi.RSSI()) + ",";
  response += "\"free_heap\":" + String(ESP.getFreeHeap()) + ",";
  response += "\"uptime_seconds\":" + String(millis() / 1000) + ",";
  response += "\"network_mode\":\"" + String(current_network == LOCAL_NETWORK ? "LOCAL" : "INTERNET") + "\",";
  response += "\"mqtt_pending\":" + String(mqtt_buffer.data_pending ? "true" : "false") + ",";
  response += "\"sheets_pending\":" + String(sheets_buffer.data_pending ? "true" : "false") + ",";
  response += "\"sheets_success\":" + String(sheets_buffer.upload_success ? "true" : "false") + ",";
  response += "\"last_update\":" + String(lastUpdateTime);
  response += "}";
  
  server.send(200, "application/json", response);
}

void handleMQTTStatus() {
  String status = "MQTT Broker: " + String(mqtt_server) + "\n";
  status += "Client ID: " + String(mqtt_client_id) + "\n";
  status += "Base Topic: " + String(mqtt_topic_base) + "\n";
  status += "Connection: " + String(mqtt_client.connected() ? "Connected" : "Disconnected") + "\n";
  status += "Data Pending: " + String(mqtt_buffer.data_pending ? "Yes" : "No") + "\n";
  status += "Network Mode: " + String(current_network == LOCAL_NETWORK ? "LOCAL" : "INTERNET") + "\n";
  status += "\n=== ENHANCED MQTT DATA (matches Google Sheets) ===\n";
  status += "Topics published:\n";
  status += "- " + String(mqtt_topic_base) + "/count_up\n";
  status += "- " + String(mqtt_topic_base) + "/count_down\n";
  status += "- " + String(mqtt_topic_base) + "/total_vehicles\n";
  status += "- " + String(mqtt_topic_base) + "/traffic_status\n";
  status += "- " + String(mqtt_topic_base) + "/timestamp\n";
  status += "- " + String(mqtt_topic_base) + "/date_time [NEW]\n";
  status += "- " + String(mqtt_topic_base) + "/device_id [NEW]\n";
  status += "- " + String(mqtt_topic_base) + "/uptime_seconds [MODIFIED]\n";
  status += "- " + String(mqtt_topic_base) + "/wifi_rssi\n";
  status += "- " + String(mqtt_topic_base) + "/ip_address\n";
  status += "- " + String(mqtt_topic_base) + "/data [ENHANCED JSON]\n";
  status += "- " + String(mqtt_topic_base) + "/system/online\n";
  status += "- " + String(mqtt_topic_base) + "/system/lcd_connected\n";
  
  server.send(200, "text/plain", status);
}

void handleSheetsStatus() {
  String status = "Google Sheets Status:\n";
  status += "Script URL: " + String(google_script_url) + "\n";
  status += "Data Pending: " + String(sheets_buffer.data_pending ? "Yes" : "No") + "\n";
  status += "Upload Attempts: " + String(sheets_buffer.upload_attempts) + "/3\n";
  status += "Last Upload Success: " + String(sheets_buffer.upload_success ? "Yes" : "No") + "\n";
  status += "Next Upload In: " + String((sheets_upload_interval - (millis() - last_sheets_upload)) / 1000) + " seconds\n";
  status += "Network Mode: " + String(current_network == LOCAL_NETWORK ? "LOCAL" : "INTERNET") + "\n";
  status += "Current Data:\n";
  status += "- Count Up: " + String(sheets_buffer.count_up) + "\n";
  status += "- Count Down: " + String(sheets_buffer.count_down) + "\n";
  status += "- Total Vehicles: " + String(sheets_buffer.total_vehicles) + "\n";
  status += "- Traffic Status: " + sheets_buffer.traffic_status + "\n";
  status += "- Timestamp: " + sheets_buffer.timestamp + "\n";
  status += "- Date Time: " + sheets_buffer.date_time + "\n";
  status += "- Device ID: " + String(mqtt_client_id) + "\n";
  status += "- WiFi RSSI: " + String(sheets_buffer.wifi_rssi) + "\n";
  status += "- Uptime Seconds: " + String(sheets_buffer.uptime / 1000) + "\n";
  
  server.send(200, "text/plain", status);
}

void handleForceSheets() {
  if (current_network != INTERNET_NETWORK) {
    server.send(200, "text/plain", "❌ Not connected to internet. Switch to internet mode first.");
    return;
  }
  
  // Force prepare data for upload
  prepareDataForGoogleSheets();
  
  // Try to upload immediately
  uploadDataToGoogleSheets();
  
  String result = "🔄 Force upload initiated.\n";
  result += "Check sheets_status for results.\n";
  result += "Upload Success: " + String(sheets_buffer.upload_success ? "Yes" : "No");
  
  server.send(200, "text/plain", result);
}

void handleTest() {
  String result = "";
  
  if (lcdConnected) {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("LCD Test OK!");
    lcd.setCursor(0, 1);
    lcd.print("Time: " + String(millis()/1000) + "s");
    result = "✓ LCD Test berhasil! Cek tampilan LCD.";
    
    for (int i = 0; i < 3; i++) {
      lcd.noBacklight();
      delay(200);
      lcd.backlight();
      delay(200);
    }
  } else {
    result = "✗ LCD tidak terhubung.";
  }
  
  server.send(200, "text/plain", result);
}

void handleReset() {
  countUp = 0;
  countDown = 0;
  lastTimestamp = "";
  lastUpdateTime = 0;
  previousTotal = 0;
  trafficStatus = "SEPI";
  mqtt_buffer.data_pending = false;
  sheets_buffer.data_pending = false;
  sheets_buffer.upload_success = false;
  sheets_buffer.upload_attempts = 0;
  
  if (lcdConnected) {
    updateLCDReady();
  }
  
  Serial.println("🔄 Counters and buffers reset");
  server.send(200, "text/plain", "Counters and buffers reset successfully");
}

// ========== UTILITY FUNCTIONS ==========
void updateTrafficStatus() {
  int currentTotal = countUp + countDown;
  int vehiclesPerMinute = currentTotal - previousTotal;
  
  trafficStatus = calculateTrafficStatus(currentTotal, vehiclesPerMinute);
  previousTotal = currentTotal;
  lastStatusUpdate = millis();
  
  Serial.println("Traffic Status Updated: " + trafficStatus + " (" + String(vehiclesPerMinute) + " vehicles/min)");
  
  if (lcdConnected) {
    updateLCD();
  }
  
  // Prepare data for MQTT (now with enhanced format)
  prepareDataForMQTT();
}

String calculateTrafficStatus(int totalVehicles, int vehiclesPerMinute) {
  if (vehiclesPerMinute <= 10) {
    return "SEPI";
  } else if (vehiclesPerMinute <= 30) {
    return "LANCAR";
  } else if (vehiclesPerMinute <= 50) {
    return "RAMAI";
  } else {
    return "PADAT";
  }
}

void testLCDConnection() {
  byte addresses[] = {0x27, 0x3F, 0x20, 0x26, 0x38};
  
  for (int i = 0; i < 5; i++) {
    Wire.beginTransmission(addresses[i]);
    if (Wire.endTransmission() == 0) {
      lcdConnected = true;
      Serial.print("✓ LCD found at address 0x");
      Serial.println(addresses[i], HEX);
      
      lcd = LiquidCrystal_I2C(addresses[i], 16, 2);
      lcd.init();
      lcd.backlight();
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print("ESP32 Counter");
      lcd.setCursor(0, 1);
      lcd.print("MQTT+Sheets!");
      delay(1000);
      return;
    }
  }
  
  lcdConnected = false;
  Serial.println("✗ LCD I2C not found!");
}

void updateLCD() {
  if (!lcdConnected) return;
  
  lcd.clear();
  
  // Baris 1: Counter
  lcd.setCursor(0, 0);
  String line1 = "UP:" + String(countUp) + " DN:" + String(countDown);
  lcd.print(line1);
  
  // Baris 2: Status dan Mode
  lcd.setCursor(0, 1);
  String mode = (current_network == LOCAL_NETWORK) ? "L" : "I";
  String sheets_indicator = sheets_buffer.data_pending ? "S" : "";
  String mqtt_indicator = mqtt_buffer.data_pending ? "M" : "";
  String line2 = trafficStatus + " [" + mode + sheets_indicator + mqtt_indicator + "]";
  lcd.print(line2);
}

void updateLCDReady() {
  if (!lcdConnected) return;
  
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Ready MQTT+GS");
  lcd.setCursor(0, 1);
  lcd.print(WiFi.localIP().toString());
}

void handleStatusIndicators() {
  // LED heartbeat setiap 2 detik
  static unsigned long lastBlink = 0;
  if (millis() - lastBlink > 2000) {
    digitalWrite(ledBuiltIn, HIGH);
    delay(50);
    digitalWrite(ledBuiltIn, LOW);
    lastBlink = millis();
  }
  
  // Check no data warning
  if (lastUpdateTime > 0 && (millis() - lastUpdateTime) > 30000) {
    Serial.println("⚠ No data received for 30 seconds");
    trafficStatus = "NO DATA";
    if (lcdConnected) {
      lcd.setCursor(0, 1);
      lcd.print("No data recv...");
    }
  }
  
  // Reset failed upload attempts after 1 hour
  if (sheets_buffer.upload_attempts >= 3 && 
      (millis() - sheets_buffer.last_upload_attempt) > 3600000) {
    Serial.println("🔄 Resetting Google Sheets upload attempts after 1 hour");
    sheets_buffer.upload_attempts = 0;
  }
}

void ledTestSequence() {
  digitalWrite(ledBuiltIn, HIGH);
  digitalWrite(ledUpPin, HIGH);
  digitalWrite(ledDownPin, HIGH);
  delay(500);
  digitalWrite(ledUpPin, LOW);
  digitalWrite(ledDownPin, LOW);
  digitalWrite(ledBuiltIn, LOW);
}

void blinkLED(int pin, int duration) {
  digitalWrite(pin, HIGH);
  delay(duration);
  digitalWrite(pin, LOW);
}