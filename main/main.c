#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "mqtt_client.h"
#include "esp_netif.h"
#include "DHT.h"
#include "driver/gpio.h"
#include <math.h>

#define DHT_GPIO GPIO_NUM_13

#define WIFI_SSID "Your_SSID"
#define WIFI_PASS "Your_Password"

static EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0

static const char *TAG_MQTT = "MQTT";
static const char *TAG_DHT22 = "DHT";

esp_mqtt_client_handle_t mqtt_client = NULL;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        ESP_LOGI(TAG_MQTT, "Disconnected. Reconnecting...");
        esp_wifi_connect();
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG_MQTT, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void wifi_init(void)
{
    wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                        IP_EVENT_STA_GOT_IP,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        &instance_got_ip));

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
}

static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;

    switch (event_id)
    {
    case MQTT_EVENT_CONNECTED:
        ESP_LOGI(TAG_MQTT, "MQTT Connected");
        break;
    case MQTT_EVENT_DISCONNECTED:
        ESP_LOGI(TAG_MQTT, "MQTT Disconnected");
        break;
    case MQTT_EVENT_PUBLISHED:
        ESP_LOGI(TAG_MQTT, "Message published (msg_id=%d)", event->msg_id);
        break;
    default:
        ESP_LOGI(TAG_MQTT, "Other MQTT event id:%ld", (long)event_id);
        break;
    }
}

void mqtt_publish_task(void *pvParameters)
{
    setDHTgpio(DHT_GPIO);
    ESP_LOGI(TAG_DHT22, "Starting DHT Task\n\n");
    vTaskDelay(pdMS_TO_TICKS(2000));
    while (1)
    {
        int ret = readDHT();

        errorHandler(ret);
        float temp = getTemperature();
        float hum = getHumidity();

        if ((!isnan(temp) && !isnan(hum)) || (temp != 0 && hum != 0))
        {
            char payload[128];
            snprintf(payload, sizeof(payload),
                     "{\"temperature\": %.2f, \"humidity\": %.2f}", temp, hum);
            ESP_LOGI(TAG_MQTT, "Published Temp: %.2f, Hum: %.2f", temp, hum);
            esp_mqtt_client_publish(mqtt_client, "sensor/dht", payload, 0, 1, 0);
        }
        else
        {
            ESP_LOGW("JSON", "Invalid value");
        }

        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_INFO);
    esp_rom_gpio_pad_select_gpio(DHT_GPIO);

    ESP_ERROR_CHECK(nvs_flash_init());
    wifi_init();

    ESP_LOGI(TAG_MQTT, "Waiting for Wi-Fi...");
    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT, false, true, portMAX_DELAY);
    ESP_LOGI(TAG_MQTT, "Wi-Fi connected");

    esp_mqtt_client_config_t mqtt_cfg = {
        .broker.address.uri = "mqtt://broker.hivemq.com",
    };

    mqtt_client = esp_mqtt_client_init(&mqtt_cfg);
    esp_mqtt_client_register_event(mqtt_client, ESP_EVENT_ANY_ID, mqtt_event_handler, NULL);
    esp_mqtt_client_start(mqtt_client);

    xTaskCreate(&mqtt_publish_task, "mqtt_publish_task", 4096, NULL, 5, NULL);
}
