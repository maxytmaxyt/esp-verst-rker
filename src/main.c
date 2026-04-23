#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "driver/gpio.h"

#define RESET_PIN 4
#define MAX_NETWORKS 5

static const char *TAG = "repeater";

httpd_handle_t server;
esp_netif_t *ap_netif;

/* ================= STORAGE ================= */

typedef struct {
    char ssid[32];
    char pass[64];
} wifi_cfg_t;

wifi_cfg_t networks[MAX_NETWORKS];
int32_t network_count = 0;

/* ================= NVS ================= */

void load_config() {
    nvs_handle_t nvs;
    if (nvs_open("cfg", NVS_READONLY, &nvs) == ESP_OK) {
        nvs_get_i32(nvs, "count", &network_count);

        for (int i = 0; i < network_count; i++) {
            char key_s[10], key_p[10];
            sprintf(key_s, "s%d", i);
            sprintf(key_p, "p%d", i);

            size_t l1 = sizeof(networks[i].ssid);
            size_t l2 = sizeof(networks[i].pass);

            nvs_get_str(nvs, key_s, networks[i].ssid, &l1);
            nvs_get_str(nvs, key_p, networks[i].pass, &l2);
        }

        nvs_close(nvs);
    }
}

void save_network(const char* s, const char* p) {
    if (network_count >= MAX_NETWORKS) network_count = 0;

    strcpy(networks[network_count].ssid, s);
    strcpy(networks[network_count].pass, p);
    network_count++;

    nvs_handle_t nvs;
    nvs_open("cfg", NVS_READWRITE, &nvs);

    nvs_set_i32(nvs, "count", network_count);

    for (int i = 0; i < network_count; i++) {
        char key_s[10], key_p[10];
        sprintf(key_s, "s%d", i);
        sprintf(key_p, "p%d", i);

        nvs_set_str(nvs, key_s, networks[i].ssid);
        nvs_set_str(nvs, key_p, networks[i].pass);
    }

    nvs_commit(nvs);
    nvs_close(nvs);
}

/* ================= WIFI ================= */

int retry_count = 0;

void connect_best_network()
{
    wifi_scan_config_t scan = {0};
    esp_wifi_scan_start(&scan, true);

    uint16_t count = 20;
    wifi_ap_record_t list[20];
    esp_wifi_scan_get_ap_records(&count, list);

    int best = -1;
    int best_rssi = -999;

    for (int i = 0; i < count; i++) {
        for (int j = 0; j < network_count; j++) {
            if (strcmp((char*)list[i].ssid, networks[j].ssid) == 0) {
                if (list[i].rssi > best_rssi) {
                    best_rssi = list[i].rssi;
                    best = j;
                }
            }
        }
    }

    if (best >= 0) {
        wifi_config_t sta = {0};
        strcpy((char*)sta.sta.ssid, networks[best].ssid);
        strcpy((char*)sta.sta.password, networks[best].pass);

        esp_wifi_set_config(WIFI_IF_STA, &sta);
        esp_wifi_connect();

        ESP_LOGI(TAG, "Connecting to best network: %s", networks[best].ssid);
    } else {
        ESP_LOGW(TAG, "No known network found");
    }
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        connect_best_network();
    }

    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (retry_count < 10) {
            retry_count++;
            connect_best_network();
        } else {
            ESP_LOGE(TAG, "Reconnect failed, fallback AP only");
        }
    }

    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        retry_count = 0;
        ESP_LOGI(TAG, "Connected!");
    }
}

void start_wifi()
{
    esp_netif_create_default_wifi_sta();
    ap_netif = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL);

    wifi_config_t ap = {
        .ap = {
            .ssid = "ESP32_Repeater",
            .password = "12345678",
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK
        }
    };

    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    esp_wifi_start();

    esp_netif_napt_enable(ap_netif);
}

/* ================= SCAN ================= */

char scan_json[4096];

void scan_networks() {
    wifi_scan_config_t scan = {0};
    esp_wifi_scan_start(&scan, true);

    uint16_t count = 20;
    wifi_ap_record_t list[20];
    esp_wifi_scan_get_ap_records(&count, list);

    strcpy(scan_json, "[");

    for (int i = 0; i < count; i++) {
        char line[128];
        sprintf(line, "{\"ssid\":\"%s\",\"rssi\":%d}%s",
                list[i].ssid, list[i].rssi,
                (i < count - 1) ? "," : "");
        strcat(scan_json, line);
    }

    strcat(scan_json, "]");
}

/* ================= WEB ================= */

esp_err_t root_get(httpd_req_t *req)
{
    const char* html =
"<!DOCTYPE html><html><head><style>"
"body{background:#121212;color:#fff;font-family:sans-serif;padding:20px}"
".card{background:#1e1e1e;padding:20px;border-radius:10px}"
"button{padding:10px;background:#00adb5;border:none;color:white}"
"</style></head><body>"
"<div class='card'>"
"<h2>ESP32 Repeater</h2>"
"<button onclick='scan()'>Scan</button><br><br>"
"<select id='net'></select><br>"
"PASS:<input id='p'><br><br>"
"<button onclick='save()'>Add Network</button>"
"</div>"
"<script>"
"function scan(){fetch('/scan').then(r=>r.json()).then(d=>{let s=document.getElementById('net');s.innerHTML='';d.sort((a,b)=>b.rssi-a.rssi);d.forEach(n=>{let o=document.createElement('option');o.text=n.ssid+' ('+n.rssi+')';o.value=n.ssid;s.add(o);});});}"
"function save(){fetch('/save',{method:'POST',body:'s='+net.value+'&p='+p.value});}"
"</script>"
"</body></html>";

    httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

esp_err_t scan_get(httpd_req_t *req)
{
    scan_networks();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, scan_json, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

esp_err_t save_post(httpd_req_t *req)
{
    char buf[200];
    int len = httpd_req_recv(req, buf, sizeof(buf));
    buf[len] = 0;

    char ssid[32] = {0}, pass[64] = {0};
    sscanf(buf, "s=%31[^&]&p=%63s", ssid, pass);

    save_network(ssid, pass);

    httpd_resp_sendstr(req, "Saved");
    return ESP_OK;
}

void start_web()
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    httpd_start(&server, &cfg);

    httpd_uri_t r = {.uri="/", .method=HTTP_GET, .handler=root_get};
    httpd_uri_t s = {.uri="/scan", .method=HTTP_GET, .handler=scan_get};
    httpd_uri_t p = {.uri="/save", .method=HTTP_POST, .handler=save_post};

    httpd_register_uri_handler(server, &r);
    httpd_register_uri_handler(server, &s);
    httpd_register_uri_handler(server, &p);
}

/* ================= RESET ================= */

void reset_task(void* arg)
{
    gpio_set_direction(RESET_PIN, GPIO_MODE_INPUT);

    while (1) {
        if (gpio_get_level(RESET_PIN) == 0) {
            vTaskDelay(4000 / portTICK_PERIOD_MS);
            if (gpio_get_level(RESET_PIN) == 0) {
                nvs_flash_erase();
                esp_restart();
            }
        }
        vTaskDelay(200 / portTICK_PERIOD_MS);
    }
}

/* ================= MAIN ================= */

void app_main(void)
{
    nvs_flash_init();
    esp_netif_init();
    esp_event_loop_create_default();

    load_config();

    start_wifi();
    start_web();

    xTaskCreate(reset_task, "reset", 2048, NULL, 5, NULL);
}
