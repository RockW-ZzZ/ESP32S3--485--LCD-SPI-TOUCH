#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "charger_service.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_log.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static esp_netif_t *sta;
static SemaphoreHandle_t lock;
static EventGroupHandle_t events;
static char saved_ssid[33];
#define CONNECTED BIT0
static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) xEventGroupClearBits(events, CONNECTED);
    if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        xEventGroupSetBits(events, CONNECTED);
        ESP_LOGI("network", "STA IP: " IPSTR, IP2STR(&e->ip_info.ip));
    }
}
static void reconnect(void *arg)
{
    (void)arg;
    for (;;) {
        xSemaphoreTake(lock, portMAX_DELAY);
        if (saved_ssid[0] && !(xEventGroupGetBits(events) & CONNECTED)) esp_wifi_connect();
        xSemaphoreGive(lock);
        vTaskDelay(pdMS_TO_TICKS(15000));
    }
}
esp_err_t charger_network_save(const char *ssid, const char *password)
{
    size_t sn = strlen(ssid), pn = strlen(password);
    if (!sn || sn > 32 || pn > 63 || (pn && pn < 8)) return ESP_ERR_INVALID_ARG;
    wifi_config_t cfg = {0};
    memcpy(cfg.sta.ssid, ssid, sn); memcpy(cfg.sta.password, password, pn);
    cfg.sta.threshold.authmode = pn ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    nvs_handle_t nvs;
    esp_err_t err = nvs_open("network", NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;
    /* One blob keeps SSID/password atomic across power loss. */
    err = nvs_set_blob(nvs, "wifi", &cfg, sizeof(cfg));
    if (err == ESP_OK) err = nvs_commit(nvs);
    nvs_close(nvs);
    if (err != ESP_OK) return err;
    xSemaphoreTake(lock, portMAX_DELAY);
    esp_wifi_disconnect(); xEventGroupClearBits(events, CONNECTED);
    err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err == ESP_OK) { snprintf(saved_ssid, sizeof(saved_ssid), "%s", ssid); err = esp_wifi_connect(); }
    xSemaphoreGive(lock);
    return err;
}
void charger_network_ssid(char ssid[33])
{
    xSemaphoreTake(lock, portMAX_DELAY); memcpy(ssid, saved_ssid, 33); xSemaphoreGive(lock);
}
void charger_network_info(char ip[16], bool *connected)
{
    *connected = (xEventGroupGetBits(events) & CONNECTED) != 0;
    esp_netif_ip_info_t info;
    if (*connected && esp_netif_get_ip_info(sta, &info) == ESP_OK)
        snprintf(ip, 16, IPSTR, IP2STR(&info.ip));
    else snprintf(ip, 16, "192.168.4.1");
}
esp_err_t charger_network_start(void)
{
    lock = xSemaphoreCreateMutex(); events = xEventGroupCreate();
    if (!lock || !events) return ESP_ERR_NO_MEM;
    esp_err_t err = esp_netif_init(); if (err != ESP_OK) return err;
    err = esp_event_loop_create_default(); if (err != ESP_OK) return err;
    esp_netif_t *ap = esp_netif_create_default_wifi_ap();
    sta = esp_netif_create_default_wifi_sta();
    if (!ap || !sta) return ESP_ERR_NO_MEM;
    esp_netif_set_hostname(sta, "charger");
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init); if (err != ESP_OK) return err;
    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL); if (err != ESP_OK) return err;
    err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, NULL); if (err != ESP_OK) return err;
    err = esp_wifi_set_storage(WIFI_STORAGE_RAM); if (err != ESP_OK) return err;
    wifi_config_t ap_cfg = {.ap = {.ssid = "ChargerCtrl", .password = "12345678", .channel = 1,
        .max_connection = 4, .authmode = WIFI_AUTH_WPA2_PSK}};
    err = esp_wifi_set_mode(WIFI_MODE_APSTA); if (err != ESP_OK) return err;
    err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg); if (err != ESP_OK) return err;
    wifi_config_t sta_cfg = {0};
    nvs_handle_t nvs;
    if (nvs_open("network", NVS_READONLY, &nvs) == ESP_OK) {
        size_t size = sizeof(sta_cfg);
        if (nvs_get_blob(nvs, "wifi", &sta_cfg, &size) != ESP_OK || size != sizeof(sta_cfg)) memset(&sta_cfg, 0, sizeof(sta_cfg));
        nvs_close(nvs);
    }
    memcpy(saved_ssid, sta_cfg.sta.ssid, 32); saved_ssid[32] = 0;
    if (saved_ssid[0]) { err = esp_wifi_set_config(WIFI_IF_STA, &sta_cfg); if (err != ESP_OK) return err; }
    err = esp_wifi_start(); if (err != ESP_OK) return err;
    /* GUI benchmarking is on core 1; network uses IDF tasks on core 0. */
    if (xTaskCreate(reconnect, "wifi_retry", 3072, NULL, 2, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    setenv("TZ", "CST-8", 1); tzset();
    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("ntp.aliyun.com");
    err = esp_netif_sntp_init(&sntp);
    if (err != ESP_OK) ESP_LOGW("network", "SNTP: %s", esp_err_to_name(err));
    ESP_LOGI("network", "AP ChargerCtrl ready: http://192.168.4.1 (redirects to web port)");
    return ESP_OK;
}
