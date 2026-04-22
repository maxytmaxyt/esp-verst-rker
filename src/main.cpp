#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <lwip/napt.h>

/* --- Configuration & Pins --- */
#define RESET_BUTTON_PIN 0 // BOOT button on most ESP32 boards
#define CONFIG_SSID "ESP32_Admin_Setup"

WebServer server(80);
Preferences preferences;

/* --- Global Settings Variables --- */
struct DeviceConfig {
  String sta_ssid;
  String sta_pass;
  String ap_ssid;
  String ap_pass;
  bool hide_ssid;
} config;

/* --- HTML Dashboard (Embedded CSS) --- */
const char INDEX_HTML[] PROGMEM = R"=====(
<!DOCTYPE html>
<html lang='en'>
<head>
    <meta charset='UTF-8'>
    <meta name='viewport' content='width=device-width, initial-scale=1.0'>
    <title>ESP32 Repeater Pro</title>
    <style>
        :root { --bg: #121212; --card: #1e1e1e; --text: #e0e0e0; --primary: #00adb5; }
        body { font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; background: var(--bg); color: var(--text); margin: 0; padding: 20px; display: flex; justify-content: center; }
        .container { width: 100%; max-width: 450px; }
        .card { background: var(--card); padding: 25px; border-radius: 15px; box-shadow: 0 10px 30px rgba(0,0,0,0.5); }
        h2 { text-align: center; color: var(--primary); margin-bottom: 25px; }
        label { display: block; margin: 10px 0 5px; font-size: 0.9em; color: #aaa; }
        input[type='text'], input[type='password'], select {
            width: 100%; padding: 12px; margin-bottom: 15px; border: 1px solid #333; border-radius: 8px;
            background: #252525; color: white; box-sizing: border-box;
        }
        .checkbox-group { display: flex; align-items: center; gap: 10px; margin: 15px 0; }
        input[type='checkbox'] { width: 18px; height: 18px; cursor: pointer; }
        .btn {
            width: 100%; padding: 15px; border: none; border-radius: 8px;
            background: var(--primary); color: white; font-weight: bold; cursor: pointer; font-size: 1em;
            transition: transform 0.2s, background 0.3s;
        }
        .btn:hover { background: #008f96; transform: translateY(-2px); }
        .footer { text-align: center; margin-top: 20px; font-size: 0.8em; color: #555; }
    </style>
</head>
<body>
    <div class='container'>
        <div class='card'>
            <h2>Repeater Settings</h2>
            <form action='/save' method='POST'>
                <label>Target Network (Uplink)</label>
                <input name='s_sta' type='text' placeholder='Router SSID' required>
                <input name='p_sta' type='password' placeholder='Router Password'>
                
                <label>Broadcast Network (Downlink)</label>
                <input name='s_ap' type='text' placeholder='New Network Name' value='ESP32_Repeater' required>
                <input name='p_ap' type='password' placeholder='New Password (min. 8 chars)'>
                
                <div class='checkbox-group'>
                    <input name='hide' type='checkbox' id='hide'>
                    <label for='hide'>Hide SSID (Hidden Network)</label>
                </div>

                <button type='submit' class='btn'>Apply Settings</button>
            </form>
        </div>
        <div class='footer'>ESP32 NAT Router &copy; 2026</div>
    </div>
</body>
</html>
)=====";

/* --- Logic Functions --- */

void loadConfig() {
    preferences.begin("wifi_store", true);
    config.sta_ssid = preferences.getString("s_sta", "");
    config.sta_pass = preferences.getString("p_sta", "");
    config.ap_ssid  = preferences.getString("s_ap", "ESP32_Repeater");
    config.ap_pass  = preferences.getString("p_ap", "");
    config.hide_ssid = preferences.getBool("hide", false);
    preferences.end();
}

void wipeMemory() {
    preferences.begin("wifi_store", false);
    preferences.clear();
    preferences.end();
    Serial.println("Memory wiped. Rebooting...");
    delay(1000);
    ESP.restart();
}

void handleSave() {
    preferences.begin("wifi_store", false);
    preferences.putString("s_sta", server.arg("s_sta"));
    preferences.putString("p_sta", server.arg("p_sta"));
    preferences.putString("s_ap", server.arg("s_ap"));
    preferences.putString("p_ap", server.arg("p_ap"));
    preferences.setBool("hide", server.hasArg("hide"));
    preferences.end();
    
    String msg = "Settings Saved. System is restarting and connecting to " + server.arg("s_sta");
    server.send(200, "text/plain", msg);
    delay(2000);
    ESP.restart();
}

void startConfigMode() {
    Serial.println("Action: Starting AP Config Mode");
    WiFi.mode(WIFI_AP);
    WiFi.softAP(CONFIG_SSID); 
    server.on("/", []() { server.send(200, "text/html", INDEX_HTML); });
    server.on("/save", HTTP_POST, handleSave);
    server.begin();
}

void startRepeaterMode() {
    Serial.printf("Action: Connecting to %s...\n", config.sta_ssid.c_str());
    WiFi.mode(WIFI_AP_STA);
    WiFi.begin(config.sta_ssid.c_str(), config.sta_pass.c_str());

    // Wait for connection with timeout
    unsigned long startAttempt = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 15000) {
        delay(500);
        Serial.print(".");
    }

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("\nFailed to connect. Reverting to Config Mode.");
        startConfigMode();
        return;
    }

    Serial.println("\nConnected! Starting NAT AP...");

    // Setup the Access Point with visibility settings
    // If ap_pass is empty, it becomes an open network
    const char* ap_p = config.ap_pass.length() >= 8 ? config.ap_pass.c_str() : NULL;
    WiFi.softAP(config.ap_ssid.c_str(), ap_p, 1, config.hide_ssid);

    // NAPT setup for routing traffic
    ip_napt_init(IP_NAPT_MAX, IP_PORT_MAX);
    ip_napt_enable_no(SOFTAP_IF, 1);
    
    Serial.println("NAT Router is online.");
}

void setup() {
    Serial.begin(115200);
    pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);

    loadConfig();

    // Trigger config mode if no SSID or reset button pressed during boot
    if (config.sta_ssid == "" || digitalRead(RESET_BUTTON_PIN) == LOW) {
        startConfigMode();
    } else {
        startRepeaterMode();
    }
}

void loop() {
    // Process WebServer requests if AP is active
    if (WiFi.getMode() & WIFI_MODE_AP) {
        server.handleClient();
    }

    // Long press reset logic
    if (digitalRead(RESET_BUTTON_PIN) == LOW) {
        delay(100); // Debounce
        unsigned long pressTime = millis();
        while (digitalRead(RESET_BUTTON_PIN) == LOW) {
            if (millis() - pressTime > 4000) {
                wipeMemory();
            }
        }
    }
}
