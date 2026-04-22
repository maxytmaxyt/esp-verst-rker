#include <stdio.h>
#include <string.h>
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "driver/gpio.h"

#define RESET_PIN 0

static const char *TAG = "repeater";

httpd_handle_t server;

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
        size_t len1 = sizeof(saved.ssid);
        size_t len2 = sizeof(saved.pass);
        nvs_get_str(nvs, "s", saved.ssid, &len1);
        nvs_get_str(nvs, "p", saved.pass, &len2);
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

esp_netif_t *ap_netif;

void start_wifi_apsta(const char* ssid, const char* pass)
{
    esp_netif_create_default_wifi_sta();
    ap_netif = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

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

    httpd_resp_sendstr(req, "OK reboot");
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

/* ================= DNS ================= */

void dns_task()
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);

    struct sockaddr_in addr;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_family = AF_INET;
    addr.sin_port = htons(53);

    bind(sock, (struct sockaddr*)&addr, sizeof(addr));

    while (1) {
        char buf[512];
        struct sockaddr_in client;
        socklen_t len = sizeof(client);

        int r = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr*)&client, &len);

        if (r > 0) {
            buf[2] |= 0x80;
            buf[3] |= 0x80;
            sendto(sock, buf, r, 0, (struct sockaddr*)&client, len);
        }
    }
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

    if (strlen(saved.ssid) > 0) {
        start_wifi_apsta(saved.ssid, saved.pass);
    } else {
        start_wifi_apsta("", "");
    }

    start_web();

    xTaskCreate(dns_task, "dns", 4096, NULL, 5, NULL);
    xTaskCreate(reset_task, "reset", 2048, NULL, 5, NULL);
}
