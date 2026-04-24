#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "lwip/ip4_addr.h"

/* ================================================
   CONFIG
   ================================================ */

#define RESET_PIN        4
#define MAX_NETWORKS     5
#define MAX_AP_RECORDS   20

#define BIT_DO_CONNECT   BIT0
#define BIT_CONNECTED    BIT1

static const char *TAG = "repeater";

/* ================================================
   GLOBALS
   ================================================ */

static httpd_handle_t       server;
static esp_netif_t         *ap_netif;
static esp_netif_t         *sta_netif;
static EventGroupHandle_t   s_wifi_eg;
static SemaphoreHandle_t    s_scan_mutex;
static int                  retry_count = 0;

/* ================================================
   STORAGE
   ================================================ */

typedef struct {
    char ssid[32];
    char pass[64];
} wifi_cfg_t;

static wifi_cfg_t networks[MAX_NETWORKS];
static int32_t    network_count = 0;

/* ================================================
   NVS
   ================================================ */

static void load_config(void)
{
    nvs_handle_t nvs;
    if (nvs_open("cfg", NVS_READONLY, &nvs) != ESP_OK) return;

    if (nvs_get_i32(nvs, "count", &network_count) != ESP_OK)
        network_count = 0;

    if (network_count > MAX_NETWORKS || network_count < 0)
        network_count = 0;

    for (int i = 0; i < network_count; i++) {
        char key_s[16], key_p[16];
        snprintf(key_s, sizeof(key_s), "s%d", i);
        snprintf(key_p, sizeof(key_p), "p%d", i);
        size_t l1 = sizeof(networks[i].ssid);
        size_t l2 = sizeof(networks[i].pass);
        nvs_get_str(nvs, key_s, networks[i].ssid, &l1);
        nvs_get_str(nvs, key_p, networks[i].pass, &l2);
    }

    nvs_close(nvs);
}

static void save_network(const char *s, const char *p)
{
    if (network_count >= MAX_NETWORKS) network_count = 0;

    strlcpy(networks[network_count].ssid, s, sizeof(networks[network_count].ssid));
    strlcpy(networks[network_count].pass, p, sizeof(networks[network_count].pass));
    network_count++;

    nvs_handle_t nvs;
    if (nvs_open("cfg", NVS_READWRITE, &nvs) != ESP_OK) return;

    nvs_set_i32(nvs, "count", network_count);
    for (int i = 0; i < network_count; i++) {
        char key_s[16], key_p[16];
        snprintf(key_s, sizeof(key_s), "s%d", i);
        snprintf(key_p, sizeof(key_p), "p%d", i);
        nvs_set_str(nvs, key_s, networks[i].ssid);
        nvs_set_str(nvs, key_p, networks[i].pass);
    }
    nvs_commit(nvs);
    nvs_close(nvs);
}

/* ================================================
   NAT / DNS HELPERS
   ================================================ */

/*
 * Fix: DNS-Server im AP-DHCP-Server setzen.
 *
 * Problem: Der AP-DHCP-Server gibt standardmäßig 192.168.4.1 als
 * DNS-Server raus. LWIP hört aber auf Port 53 NICHT → DNS-Anfragen
 * von AP-Clients laufen ins Leere → kein Internet trotz funktionierendem
 * Routing.
 *
 * Lösung: Im DHCP-Server den vom Router erhaltenen DNS eintragen.
 * Als Fallback 8.8.8.8, damit es auch ohne frischen DHCP-Lease klappt.
 */
static void update_ap_dns(void)
{
    esp_netif_dns_info_t dns = {0};
    bool got_dns = false;

    /* Versuche, den DNS-Server der STA-Verbindung zu übernehmen */
    if (sta_netif &&
        esp_netif_get_dns_info(sta_netif, ESP_NETIF_DNS_MAIN, &dns) == ESP_OK &&
        dns.ip.u_addr.ip4.addr != 0) {
        got_dns = true;
        ESP_LOGI(TAG, "Using upstream DNS: " IPSTR,
                 IP2STR(&dns.ip.u_addr.ip4));
    }

    /* Fallback auf Google-DNS */
    if (!got_dns) {
        IP4_ADDR(&dns.ip.u_addr.ip4, 8, 8, 8, 8);
        dns.ip.type = ESP_IPADDR_TYPE_V4;
        ESP_LOGW(TAG, "Using fallback DNS: 8.8.8.8");
    }

    /* DHCP-Server kurz stoppen, DNS-Option setzen, neu starten */
    esp_netif_dhcps_stop(ap_netif);

    esp_netif_dhcps_option(ap_netif,
                           ESP_NETIF_OP_SET,
                           ESP_NETIF_DOMAIN_NAME_SERVER,
                           &dns.ip.u_addr.ip4.addr,
                           sizeof(dns.ip.u_addr.ip4.addr));

    esp_netif_dhcps_start(ap_netif);
    ESP_LOGI(TAG, "AP DHCP DNS updated");
}

/*
 * Fix: NAPT erst aktivieren, wenn STA eine IP hat.
 *
 * Problem: esp_netif_napt_enable() direkt nach esp_wifi_start()
 * aufzurufen bewirkt nichts sinnvolles, weil die STA-Route noch
 * nicht existiert. NAPT braucht eine aktive IP auf dem STA-Interface
 * als "Uplink", auf das es NATten kann.
 */
static void enable_nat(void)
{
    esp_netif_napt_enable(ap_netif);
    ESP_LOGI(TAG, "NAT enabled");
}

static void disable_nat(void)
{
    esp_netif_napt_disable(ap_netif);
    ESP_LOGI(TAG, "NAT disabled");
}

/* ================================================
   WIFI CORE
   ================================================ */

static void do_scan_and_connect(void)
{
    if (xSemaphoreTake(s_scan_mutex, pdMS_TO_TICKS(8000)) != pdTRUE) {
        ESP_LOGW(TAG, "Scan mutex timeout");
        return;
    }

    wifi_ap_record_t *list = malloc(MAX_AP_RECORDS * sizeof(wifi_ap_record_t));
    if (!list) {
        ESP_LOGE(TAG, "malloc AP list failed");
        xSemaphoreGive(s_scan_mutex);
        return;
    }

    wifi_scan_config_t scan_cfg = {0};
    esp_err_t err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Scan failed: %s", esp_err_to_name(err));
        free(list);
        xSemaphoreGive(s_scan_mutex);
        return;
    }

    uint16_t count = MAX_AP_RECORDS;
    esp_wifi_scan_get_ap_records(&count, list);

    int best = -1, best_rssi = -999;
    for (int i = 0; i < count; i++) {
        for (int j = 0; j < network_count; j++) {
            if (strcmp((char *)list[i].ssid, networks[j].ssid) == 0 &&
                list[i].rssi > best_rssi) {
                best_rssi = list[i].rssi;
                best = j;
            }
        }
    }

    free(list);
    xSemaphoreGive(s_scan_mutex);

    if (best >= 0) {
        wifi_config_t sta = {0};
        strlcpy((char *)sta.sta.ssid,     networks[best].ssid, sizeof(sta.sta.ssid));
        strlcpy((char *)sta.sta.password, networks[best].pass, sizeof(sta.sta.password));
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_wifi_set_config(WIFI_IF_STA, &sta);
        esp_wifi_connect();
        ESP_LOGI(TAG, "Connecting to: %s (RSSI %d)", networks[best].ssid, best_rssi);
    } else {
        ESP_LOGW(TAG, "No known network in range");
    }
}

/* ================================================
   WIFI MANAGER TASK
   ================================================ */

static void wifi_manager_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1500));
    do_scan_and_connect();

    while (1) {
        EventBits_t bits = xEventGroupWaitBits(
            s_wifi_eg, BIT_DO_CONNECT,
            pdTRUE, pdFALSE, portMAX_DELAY);

        if (bits & BIT_DO_CONNECT) {
            int delay_s = (retry_count < 10) ? retry_count * 3 : 30;
            if (delay_s > 0) {
                ESP_LOGI(TAG, "Reconnect in %d s (retry %d)", delay_s, retry_count);
                vTaskDelay(pdMS_TO_TICKS(delay_s * 1000));
            }
            do_scan_and_connect();
        }
    }
}

/* ================================================
   WIFI EVENT HANDLER
   ================================================ */

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_wifi_eg, BIT_CONNECTED);

        /* Fix: NAT deaktivieren, solange kein Uplink vorhanden */
        disable_nat();

        if (retry_count < 15) {
            retry_count++;
            xEventGroupSetBits(s_wifi_eg, BIT_DO_CONNECT);
        } else {
            ESP_LOGE(TAG, "Max retries reached – AP-only mode");
        }

    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        retry_count = 0;
        xEventGroupSetBits(s_wifi_eg, BIT_CONNECTED);

        /*
         * Fix: NAT + DNS erst hier aktivieren/aktualisieren.
         * Jetzt hat das STA-Interface eine IP und eine Route ins Internet →
         * NAPT kann sinnvoll NATten, und der upstream DNS ist bekannt.
         */
        update_ap_dns();
        enable_nat();

        ip_event_got_ip_t *event = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "STA connected – IP: " IPSTR,
                 IP2STR(&event->ip_info.ip));
    }
}

/* ================================================
   WIFI INIT
   ================================================ */

static void start_wifi(void)
{
    s_wifi_eg    = xEventGroupCreate();
    s_scan_mutex = xSemaphoreCreateMutex();

    sta_netif = esp_netif_create_default_wifi_sta();
    ap_netif  = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, NULL));

    wifi_config_t ap_cfg = {
        .ap = {
            .ssid           = "ESP32_Repeater",
            .password       = "12345678",
            .max_connection = 4,
            .authmode       = WIFI_AUTH_WPA_WPA2_PSK,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    /*
     * Fix: Kein esp_netif_napt_enable() hier!
     * Wird erst in IP_EVENT_STA_GOT_IP aktiviert.
     * Initialen DNS-Fallback (8.8.8.8) schon jetzt setzen,
     * damit Clients, die sich vor dem ersten STA-Connect verbinden,
     * bereits einen DNS-Eintrag haben.
     */
    update_ap_dns();

    xTaskCreate(wifi_manager_task, "wifi_mgr", 8192, NULL, 5, NULL);
}

/* ================================================
   HTTP SCAN
   ================================================ */

static char scan_json[4096];

static void scan_networks(void)
{
    if (xSemaphoreTake(s_scan_mutex, pdMS_TO_TICKS(10000)) != pdTRUE) {
        strlcpy(scan_json, "[]", sizeof(scan_json));
        return;
    }

    wifi_ap_record_t *list = malloc(MAX_AP_RECORDS * sizeof(wifi_ap_record_t));
    if (!list) {
        strlcpy(scan_json, "[]", sizeof(scan_json));
        xSemaphoreGive(s_scan_mutex);
        return;
    }

    wifi_scan_config_t scan_cfg = {0};
    if (esp_wifi_scan_start(&scan_cfg, true) != ESP_OK) {
        strlcpy(scan_json, "[]", sizeof(scan_json));
        free(list);
        xSemaphoreGive(s_scan_mutex);
        return;
    }

    uint16_t count = MAX_AP_RECORDS;
    esp_wifi_scan_get_ap_records(&count, list);

    int off = 0;
    off += snprintf(scan_json + off, sizeof(scan_json) - off, "[");
    for (int i = 0; i < count; i++) {
        if (off >= (int)sizeof(scan_json) - 80) break;
        off += snprintf(scan_json + off, sizeof(scan_json) - off,
                        "{\"ssid\":\"%s\",\"rssi\":%d}%s",
                        list[i].ssid, list[i].rssi,
                        (i < count - 1) ? "," : "");
    }
    snprintf(scan_json + off, sizeof(scan_json) - off, "]");

    free(list);
    xSemaphoreGive(s_scan_mutex);
}

/* ================================================
   HTTP HANDLER
   ================================================ */

static esp_err_t root_get(httpd_req_t *req)
{
    const char *html =
        "<!DOCTYPE html><html><head><style>"
        "body{background:#121212;color:#fff;font-family:sans-serif;padding:20px}"
        ".card{background:#1e1e1e;padding:20px;border-radius:10px}"
        "button{padding:10px;background:#00adb5;border:none;color:white;cursor:pointer}"
        "select,input{margin:6px 0;padding:6px;width:100%;box-sizing:border-box}"
        "</style></head><body>"
        "<div class='card'>"
        "<h2>ESP32 Repeater</h2>"
        "<button onclick='scan()'>Scan</button><br><br>"
        "<select id='net'></select>"
        "<input id='p' placeholder='Password'><br><br>"
        "<button onclick='save()'>Add Network</button>"
        "<p id='msg'></p>"
        "</div>"
        "<script>"
        "function scan(){"
        "document.getElementById('msg').innerText='Scanning...';"
        "fetch('/scan').then(r=>r.json()).then(d=>{"
        "let s=document.getElementById('net');s.innerHTML='';"
        "d.sort((a,b)=>b.rssi-a.rssi);"
        "d.forEach(n=>{let o=document.createElement('option');"
        "o.text=n.ssid+' ('+n.rssi+' dBm)';o.value=n.ssid;s.add(o);});"
        "document.getElementById('msg').innerText='Found '+d.length+' networks';});"
        "}"
        "function save(){"
        "let n=document.getElementById('net');"
        "let p=document.getElementById('p');"
        "fetch('/save',{method:'POST',"
        "body:'s='+encodeURIComponent(n.value)+'&p='+encodeURIComponent(p.value)})"
        ".then(r=>r.text()).then(t=>document.getElementById('msg').innerText=t);"
        "}"
        "</script>"
        "</body></html>";

    httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t scan_get(httpd_req_t *req)
{
    scan_networks();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, scan_json, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t save_post(httpd_req_t *req)
{
    char buf[200];
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad request");
        return ESP_FAIL;
    }
    buf[len] = '\0';

    char ssid[32] = {0}, pass[64] = {0};
    sscanf(buf, "s=%31[^&]&p=%63s", ssid, pass);

    if (ssid[0] == '\0') {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing SSID");
        return ESP_FAIL;
    }

    save_network(ssid, pass);
    retry_count = 0;
    xEventGroupSetBits(s_wifi_eg, BIT_DO_CONNECT);

    httpd_resp_sendstr(req, "Saved – connecting...");
    return ESP_OK;
}

static void start_web(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    httpd_start(&server, &cfg);

    httpd_uri_t r = {.uri = "/",     .method = HTTP_GET,  .handler = root_get};
    httpd_uri_t s = {.uri = "/scan", .method = HTTP_GET,  .handler = scan_get};
    httpd_uri_t p = {.uri = "/save", .method = HTTP_POST, .handler = save_post};

    httpd_register_uri_handler(server, &r);
    httpd_register_uri_handler(server, &s);
    httpd_register_uri_handler(server, &p);
}

/* ================================================
   RESET TASK
   ================================================ */

static void reset_task(void *arg)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << RESET_PIN),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io_conf);

    while (1) {
        if (gpio_get_level(RESET_PIN) == 0) {
            vTaskDelay(pdMS_TO_TICKS(4000));
            if (gpio_get_level(RESET_PIN) == 0) {
                ESP_LOGW(TAG, "Factory reset triggered!");
                nvs_flash_erase();
                esp_restart();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

/* ================================================
   MAIN
   ================================================ */

void app_main(void)
{
    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS invalid – erasing");
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    load_config();
    start_wifi();
    start_web();

    xTaskCreate(reset_task, "reset", 4096, NULL, 3, NULL);
}
