#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <lwip/napt.h>

#define RESET_BUTTON_PIN 0 
#define CONFIG_SSID "ESP32_Setup"

WebServer server(80);
Preferences preferences;

String ssid_sta, pass_sta, ssid_ap, pass_ap;

void startConfigMode();
void startRepeaterMode();
void wipeMemory();

void handleRoot() {
  String html = "<h1>ESP32 WiFi Config</h1><form action='/save' method='POST'>";
  html += "Connect to SSID: <input name='s_sta'><br>";
  html += "Connect to Password: <input name='p_sta'><br>";
  html += "Broadcast SSID: <input name='s_ap'><br>";
  html += "Broadcast Password: <input name='p_ap'><br>";
  html += "<input type='submit' value='Save'></form>";
  server.send(200, "text/html", html);
}

void handleSave() {
  preferences.begin("wifi", false);
  preferences.putString("s_sta", server.arg("s_sta"));
  preferences.putString("p_sta", server.arg("p_sta"));
  preferences.putString("s_ap", server.arg("s_ap"));
  preferences.putString("p_ap", server.arg("p_ap"));
  preferences.end();
  
  server.send(200, "text/plain", "Settings saved. Restarting...");
  delay(2000);
  ESP.restart();
}

void setup() {
  Serial.begin(115200);
  pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);

  preferences.begin("wifi", true);
  ssid_sta = preferences.getString("s_sta", "");
  pass_sta = preferences.getString("p_sta", "");
  ssid_ap = preferences.getString("s_ap", "ESP32_Repeater");
  pass_ap = preferences.getString("p_ap", ""); 
  preferences.end();

  if (digitalRead(RESET_BUTTON_PIN) == LOW || ssid_sta == "") {
    startConfigMode();
  } else {
    startRepeaterMode();
  }
}

void startConfigMode() {
  Serial.println("Entering Config Mode...");
  WiFi.softAP(CONFIG_SSID); 
  server.on("/", handleRoot);
  server.on("/save", handleSave);
  server.begin();
}

void startRepeaterMode() {
  Serial.println("Starting Repeater...");
  
  WiFi.mode(WIFI_AP_STA);
  WiFi.begin(ssid_sta.c_str(), pass_sta.c_str());

  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    if (digitalRead(RESET_BUTTON_PIN) == LOW) wipeMemory();
  }

  // Initialize the Access Point using the stored credentials
  // If pass_ap is empty, the network will be unencrypted
  WiFi.softAP(ssid_ap.c_str(), pass_ap.length() > 0 ? pass_ap.c_str() : NULL);

  // NAPT (Network Address Port Translation) configuration
  // This enables IP packet routing between the STA interface and the AP interface
  ip_napt_init(IP_NAPT_MAX, IP_PORT_MAX);
  ip_napt_enable_no(SOFTAP_IF, 1);
}

void wipeMemory() {
  preferences.begin("wifi", false);
  preferences.clear();
  preferences.end();
  Serial.println("Memory wiped. Restarting...");
  delay(1000);
  ESP.restart();
}

void loop() {
  if (WiFi.getMode() == WIFI_MODE_AP) {
    server.handleClient();
  }
  
  if (digitalRead(RESET_BUTTON_PIN) == LOW) {
    // Debounce/hold delay to ensure intentional reset
    delay(3000); 
    if (digitalRead(RESET_BUTTON_PIN) == LOW) {
      wipeMemory();
    }
  }
}
