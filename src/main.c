#include <stdio.h>
#include <string.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "driver/gpio.h"

#define RESET_PIN 4

static const char *TAG = "repeater";

httpd_handle_t server;
esp_netif_t *ap_netif;

/* ================= CONFIG ================= */

typedef struct {
    char ssid[32];
    char pass[64];
} wifi_cfg_t;

wifi_cfg_t saved;

/* ================= NVS ================= */

void load_config() {
    nvs_handle_t nvs;
    if (nvs_open("cfg", NVS_READONLY, &nvs) == ESP_OK) {
        size_t l1 = sizeof(saved.ssid);
        size_t l2 = sizeof(saved.pass);
        nvs_get_str(nvs, "s", saved.ssid, &l1);
        nvs_get_str(nvs, "p", saved.pass, &l2);
        nvs_close(nvs);
    }
}

void save_config(const char* s, const char* p) {
    nvs_handle_t nvs;
    nvs_open("cfg", NVS_READWRITE, &nvs);
    nvs_set_str(nvs, "s", s);
    nvs_set_str(nvs, "p", p);
    nvs_commit(nvs);
    nvs_close(nvs);
}

/* ================= WIFI ================= */

static int retry_count = 0;

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                               int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    }

    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (retry_count < 10) {
            esp_wifi_connect();
            retry_count++;
            ESP_LOGW(TAG, "Reconnect attempt %d", retry_count);
        } else {
            ESP_LOGE(TAG, "Reconnect failed, restarting...");
            esp_restart();
        }
    }

    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        retry_count = 0;
        ESP_LOGI(TAG, "Connected with IP");
    }
}

void start_wifi_apsta(const char* ssid, const char* pass)
{
    esp_netif_create_default_wifi_sta();
    ap_netif = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL);

    wifi_config_t sta = {0};
    strcpy((char*)sta.sta.ssid, ssid);
    strcpy((char*)sta.sta.password, pass);

    wifi_config_t ap = {
        .ap = {
            .ssid = "ESP32_Repeater",
            .password = "12345678",
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK
        }
    };

    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_STA, &sta);
    esp_wifi_set_config(WIFI_IF_AP, &ap);

    esp_wifi_start();

    esp_netif_napt_enable(ap_netif);
}

/* ================= SCAN ================= */

char scan_json[2048];

void scan_networks() {
    wifi_scan_config_t scan = {0};
    esp_wifi_scan_start(&scan, true);

    uint16_t count = 20;
    wifi_ap_record_t list[20];
    esp_wifi_scan_get_ap_records(&count, list);

    strcpy(scan_json, "[");

    for (int i = 0; i < count; i++) {
        char line[128];
        sprintf(line, "\"%s\"%s", list[i].ssid, (i < count - 1) ? "," : "");
        strcat(scan_json, line);
    }

    strcat(scan_json, "]");
}

/* ================= WEB ================= */

esp_err_t root_get(httpd_req_t *req)
{
    const char* html =
    "<!DOCTYPE html><html><body>"
    "<h2>ESP32 Repeater</h2>"
    "<button onclick='scan()'>Scan</button><br><br>"
    "<select id='net'></select><br>"
    "PASS:<input id='p'><br>"
    "<button onclick='save()'>Connect</button>"
    "<script>"
    "function scan(){fetch('/scan').then(r=>r.json()).then(d=>{let s=document.getElementById('net');s.innerHTML='';d.forEach(n=>{let o=document.createElement('option');o.text=n;s.add(o);});});}"
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

    save_config(ssid, pass);

    httpd_resp_sendstr(req, "Saved. Rebooting...");
    esp_restart();
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

    start_wifi_apsta(saved.ssid, saved.pass);

    start_web();

    xTaskCreate(reset_task, "reset", 2048, NULL, 5, NULL);
}
