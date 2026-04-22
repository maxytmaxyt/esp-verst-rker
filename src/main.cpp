#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>

/* * FORCE NAPT DEFINITIONS
 * These must come before the lwip includes to ensure the headers 
 * actually expose the functions.
 */
#undef IP_NAPT
#define IP_NAPT 1

extern "C" {
  #include <lwip/ip4_napt.h>
  #include <lwip/err.h>
}

/* --- Constants --- */
#define RESET_BUTTON_PIN 0  // BOOT button
#define CONFIG_SSID "ESP32_Admin_Setup"

// On ESP32: Station is interface 0, SoftAP is interface 1
#define NAPT_IFACE_SOFTAP 1 

#ifndef IP_NAPT_MAX
  #define IP_NAPT_MAX 512
#endif
#ifndef IP_PORT_MAX
  #define IP_PORT_MAX 512
#endif

WebServer server(80);
Preferences preferences;

struct DeviceConfig {
    String sta_ssid;
    String sta_pass;
    String ap_ssid;
    String ap_pass;
    bool hide_ssid;
} config;

/* --- UI / HTML --- */
String buildIndexHtml() {
    String html = F(R"=====(
<!DOCTYPE html>
<html lang='en'>
<head>
    <meta charset='UTF-8'>
    <meta name='viewport' content='width=device-width, initial-scale=1.0'>
    <title>ESP32 Repeater Pro</title>
    <style>
        :root { --bg: #121212; --card: #1e1e1e; --text: #e0e0e0; --primary: #00adb5; }
        body { font-family: 'Segoe UI', sans-serif; background: var(--bg); color: var(--text); margin: 0; padding: 20px; display: flex; justify-content: center; }
        .container { width: 100%; max-width: 450px; }
        .card { background: var(--card); padding: 25px; border-radius: 15px; box-shadow: 0 10px 30px rgba(0,0,0,0.5); }
        h2 { text-align: center; color: var(--primary); margin-bottom: 25px; }
        label { display: block; margin: 10px 0 5px; font-size: 0.9em; color: #aaa; }
        input[type='text'], input[type='password'] {
            width: 100%; padding: 12px; margin-bottom: 15px; border: 1px solid #333; border-radius: 8px;
            background: #252525; color: white; box-sizing: border-box;
        }
        .checkbox-group { display: flex; align-items: center; gap: 10px; margin: 15px 0; }
        .btn {
            width: 100%; padding: 15px; border: none; border-radius: 8px;
            background: var(--primary); color: white; font-weight: bold; cursor: pointer; font-size: 1em;
            transition: 0.3s;
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
                <input name='s_sta' type='text' placeholder='Router SSID' value=)=====");
    html += "\"" + config.sta_ssid + "\"";
    html += F(R"=====( required>
                <input name='p_sta' type='password' placeholder='Router Password'>

                <label>Broadcast Network (Downlink)</label>
                <input name='s_ap' type='text' placeholder='New Network Name' value=)=====");
    html += "\"" + config.ap_ssid + "\"";
    html += F(R"=====( required>
                <input name='p_ap' type='password' placeholder='New Password'>

                <div class='checkbox-group'>
                    <input name='hide' type='checkbox' id='hide')=====");
    
    if (config.hide_ssid) html += F(" checked");
    
    html += F(R"=====(>
                    <label for='hide'>Hide SSID</label>
                </div>
                <button type='submit' class='btn'>Apply Settings</button>
            </form>
        </div>
        <div class='footer'>ESP32 NAT Router &copy; 2026</div>
    </div>
</body>
</html>
)=====");
    return html;
}

/* --- Logic --- */

void loadConfig() {
    preferences.begin("wifi_store", true);
    config.sta_ssid  = preferences.getString("s_sta", "");
    config.sta_pass  = preferences.getString("p_sta", "");
    config.ap_ssid   = preferences.getString("s_ap", "ESP32_Repeater");
    config.ap_pass   = preferences.getString("p_ap", "");
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
    preferences.putString("s_ap",  server.arg("s_ap"));
    preferences.putString("p_ap",  server.arg("p_ap"));
    preferences.putBool("hide", server.hasArg("hide"));
    preferences.end();

    server.send(200, "text/plain", "Settings saved. Restarting...");
    delay(2000);
    ESP.restart();
}

void startConfigMode() {
    Serial.println("Starting AP Config Mode...");
    WiFi.mode(WIFI_AP);
    WiFi.softAP(CONFIG_SSID);
    server.on("/", []() { server.send(200, "text/html", buildIndexHtml()); });
    server.on("/save", HTTP_POST, handleSave);
    server.begin();
    Serial.printf("Config portal at: http://%s\n", WiFi.softAPIP().toString().c_str());
}

void startRepeaterMode() {
    Serial.printf("Connecting to %s...\n", config.sta_ssid.c_str());
    WiFi.mode(WIFI_AP_STA);
    WiFi.begin(config.sta_ssid.c_str(), config.sta_pass.c_str());

    unsigned long startAttempt = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startAttempt < 15000) {
        delay(500);
        Serial.print(".");
    }

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("\nConnection failed. Starting Config Mode.");
        startConfigMode();
        return;
    }

    Serial.printf("\nConnected. IP: %s\n", WiFi.localIP().toString().c_str());

    // Start the Access Point
    const char* ap_p = (config.ap_pass.length() >= 8) ? config.ap_pass.c_str() : nullptr;
    WiFi.softAP(config.ap_ssid.c_str(), ap_p, 1, config.hide_ssid ? 1 : 0);

    // Initialize the NAPT table
    ip_napt_init(IP_NAPT_MAX, IP_PORT_MAX);
    
    // Enable NAPT on the SoftAP interface (Interface index 1)
    if (ip_napt_enable_no(NAPT_IFACE_SOFTAP, 1) == ERR_OK) {
        Serial.println("NAPT enabled successfully.");
    } else {
        Serial.println("Failed to enable NAPT.");
    }
}

void setup() {
    Serial.begin(115200);
    delay(500); // Give serial some time
    pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);

    loadConfig();

    if (config.sta_ssid == "" || digitalRead(RESET_BUTTON_PIN) == LOW) {
        startConfigMode();
    } else {
        startRepeaterMode();
    }
}

void loop() {
    // WebServer runs in both modes to allow reconfiguration
    server.handleClient();

    // Factory Reset check
    if (digitalRead(RESET_BUTTON_PIN) == LOW) {
        delay(50); 
        unsigned long pressStart = millis();
        while (digitalRead(RESET_BUTTON_PIN) == LOW) {
            if (millis() - pressStart > 4000) {
                wipeMemory();
            }
        }
    }
}
