#include <stdio.h>
#include <string.h>
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"

#define AP_SSID "ESP32_Repeater"
#define AP_PASS "12345678"

static const char *TAG = "repeater";

httpd_handle_t server = NULL;

static esp_err_t root_get_handler(httpd_req_t *req)
{
    const char* resp =
        "<!DOCTYPE html><html><body>"
        "<h2>ESP32 Repeater</h2>"
        "<form method='POST' action='/save'>"
        "SSID:<input name='s'><br>"
        "PASS:<input name='p'><br>"
        "<input type='submit'></form>"
        "</body></html>";

    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t save_post_handler(httpd_req_t *req)
{
    char buf[100];
    int len = httpd_req_recv(req, buf, sizeof(buf));
    if (len > 0) {
        buf[len] = 0;
        ESP_LOGI(TAG, "DATA: %s", buf);
    }

    httpd_resp_sendstr(req, "Saved. Reboot...");
    esp_restart();
    return ESP_OK;
}

void start_web()
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    httpd_start(&server, &config);

    httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get_handler
    };

    httpd_uri_t save = {
        .uri = "/save",
        .method = HTTP_POST,
        .handler = save_post_handler
    };

    httpd_register_uri_handler(server, &root);
    httpd_register_uri_handler(server, &save);
}

void start_wifi()
{
    esp_netif_t *sta = esp_netif_create_default_wifi_sta();
    esp_netif_t *ap  = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    wifi_config_t ap_config = {
        .ap = {
            .ssid = AP_SSID,
            .password = AP_PASS,
            .max_connection = 4,
            .authmode = WIFI_AUTH_WPA_WPA2_PSK
        }
    };

    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    esp_wifi_start();

    esp_netif_napt_enable(ap);
}

void start_dns()
{
    int sock = socket(AF_INET, SOCK_DGRAM, 0);

    struct sockaddr_in addr;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_family = AF_INET;
    addr.sin_port = htons(53);

    bind(sock, (struct sockaddr*)&addr, sizeof(addr));

    while (1) {
        char buffer[512];
        struct sockaddr_in client;
        socklen_t len = sizeof(client);

        int r = recvfrom(sock, buffer, sizeof(buffer), 0,
                         (struct sockaddr*)&client, &len);

        if (r > 0) {
            buffer[2] |= 0x80;
            buffer[3] |= 0x80;
            sendto(sock, buffer, r, 0,
                   (struct sockaddr*)&client, len);
        }
    }
}

void app_main(void)
{
    nvs_flash_init();
    esp_netif_init();
    esp_event_loop_create_default();

    start_wifi();
    start_web();

    xTaskCreate(start_dns, "dns", 4096, NULL, 5, NULL);
}
