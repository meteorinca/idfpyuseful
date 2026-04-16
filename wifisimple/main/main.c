#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "cJSON.h"

// ==================== WiFi Configuration ====================
// WiFi credentials
#define WIFI_SSID "balls"
#define WIFI_PASS "balls"

// ==================== LED Configuration ====================
#define LED_GPIO         GPIO_NUM_21   // Built‑in LED (active low)
#define LED_ON           0
#define LED_OFF          1

// ==================== Servo Configuration ====================
#define SERVO1_GPIO      GPIO_NUM_40    // Adjust to your wiring
#define SERVO2_GPIO      GPIO_NUM_39
#define LEDC_TIMER       LEDC_TIMER_0
#define LEDC_CH_SERVO1   LEDC_CHANNEL_0
#define LEDC_CH_SERVO2   LEDC_CHANNEL_1
#define SERVO_MIN_PULSE  500   // µs for 0°
#define SERVO_MAX_PULSE  2500  // µs for 180°

// ==================== 433MHz Configuration ====================
#define RF_RX_GPIO       GPIO_NUM_41    // DATA pin of receiver module
#define RF_TX_GPIO       GPIO_NUM_42    // DATA pin of transmitter module

// Use the new component
#include "rcswitch.h"
static RCSWITCH_t rc; // Create an instance

// ==================== Global Variables ====================
static EventGroupHandle_t wifi_event_group;
#define WIFI_CONNECTED_BIT BIT0
static uint8_t last_ip_digit1 = 0, last_ip_digit2 = 0;
static httpd_handle_t server = NULL;

// ==================== LED & Servo Functions ====================
void led_set(bool on) {
    gpio_set_level(LED_GPIO, on ? LED_ON : LED_OFF);
}

void servo_init(void) {
    ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_14_BIT,
        .timer_num = LEDC_TIMER,
        .freq_hz = 50,          // 20ms period
        .clk_cfg = LEDC_AUTO_CLK
    };
    ledc_timer_config(&timer);

    ledc_channel_config_t ch = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_sel = LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
        .gpio_num = SERVO1_GPIO
    };
    ch.channel = LEDC_CH_SERVO1;
    ledc_channel_config(&ch);
    ch.gpio_num = SERVO2_GPIO;
    ch.channel = LEDC_CH_SERVO2;
    ledc_channel_config(&ch);
}

void servo_set_angle(int servo_num, int angle) {
    if (angle < 0) angle = 0;
    if (angle > 180) angle = 180;
    uint32_t pulse_us = SERVO_MIN_PULSE + (angle * (SERVO_MAX_PULSE - SERVO_MIN_PULSE) / 180);
    uint32_t duty = (pulse_us * (1 << 14)) / 20000;  // 20ms period
    ledc_channel_t ch = (servo_num == 1) ? LEDC_CH_SERVO1 : LEDC_CH_SERVO2;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, ch, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, ch);
}

// ==================== 433MHz Send Confirmation ====================
void send_confirmation_code(uint32_t original_code) {
    uint32_t confirm_code = original_code ^ 0xAAAAAA;  // simple transformation
    sendCode(&rc, confirm_code, 24); // Use rc.send() method
    ESP_LOGI("RF", "Sent confirmation code 0x%06X", confirm_code);
    vTaskDelay(pdMS_TO_TICKS(500));
}


// ==================== Action Handlers (called by web or RF) ====================
void action_toggle_led(void) {
    static bool led_state = false;
    led_state = !led_state;
    led_set(led_state);
    ESP_LOGI("ACTION", "LED toggled to %s", led_state ? "ON" : "OFF");
}

void action_set_servo(int servo, int angle) {
    servo_set_angle(servo, angle);
    ESP_LOGI("ACTION", "Servo%d set to %d°", servo, angle);
}

// ==================== 433MHz Receiver Task ====================
// ==================== 433MHz Receiver Task ====================
void rf_receiver_task(void *pvParameters) {
    enableReceive(&rc, RF_RX_GPIO); // Set up the receiver
    while (1) {
        if (available(&rc)) { // Check if a code is available
            uint32_t code = getReceivedValue(&rc); // Get the received code
            resetAvailable(&rc); // Reset for the next read
            
            ESP_LOGI("RF", "Received code: 0x%06X", code);
            
            // Map received codes to actions (customize as needed)
            switch (code) {
                case 0x123456:
                    action_toggle_led();
                    send_confirmation_code(code);
                    break;
                case 0x234567:
                    action_set_servo(1, 0);
                    send_confirmation_code(code);
                    break;
                // ... other cases ...
                default:
                    ESP_LOGW("RF", "Unknown code, ignoring");
                    break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// ==================== Web Server ====================
static esp_err_t root_get_handler(httpd_req_t *req) {
    const char html[] = 
        "<!DOCTYPE html><html><head><title>MOJ ESP32 Control</title>"
        "<style>body{font-family:sans-serif;} button{font-size:20px; margin:5px;}</style></head>"
        "<body><h1>MOJ ESP32 Control</h1>"
        "<h2>LED</h2><button onclick=\"fetch('/led?state=toggle')\">Toggle LED</button>"
        "<h2>Servo 1</h2>"
        "<button onclick=\"fetch('/servo?num=1&angle=0')\">0</button>"
        "<button onclick=\"fetch('/servo?num=1&angle=90')\">90</button>"
        "<button onclick=\"fetch('/servo?num=1&angle=180')\">180</button>"
        "<h2>Servo 2</h2>"
        "<button onclick=\"fetch('/servo?num=2&angle=0')\">0</button>"
        "<button onclick=\"fetch('/servo?num=2&angle=90')\">90</button>"
        "<button onclick=\"fetch('/servo?num=2&angle=180')\">180</button>"
        "<h2>Send custom RF code</h2>"
        "<input id='code' placeholder='hex code e.g. 123456'>"
        "<button onclick=\"fetch('/send?code='+document.getElementById('code').value)\">Send</button>"
        "</body></html>";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html, strlen(html));
    return ESP_OK;
}

static esp_err_t led_handler(httpd_req_t *req) {
    char buf[100];
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char param[32];
        if (httpd_query_key_value(buf, "state", param, sizeof(param)) == ESP_OK) {
            if (strcmp(param, "toggle") == 0) {
                action_toggle_led();
            }
        }
    }
    httpd_resp_send(req, "OK", 2);
    return ESP_OK;
}

static esp_err_t servo_handler(httpd_req_t *req) {
    char buf[100];
    int servo = 0, angle = 0;
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char param[16];
        if (httpd_query_key_value(buf, "num", param, sizeof(param)) == ESP_OK)
            servo = atoi(param);
        if (httpd_query_key_value(buf, "angle", param, sizeof(param)) == ESP_OK)
            angle = atoi(param);
    }
    if (servo >= 1 && servo <= 2) {
        action_set_servo(servo, angle);
    }
    httpd_resp_send(req, "OK", 2);
    return ESP_OK;
}

static esp_err_t send_rf_handler(httpd_req_t *req) {
    char buf[100];
    uint32_t code = 0;
    if (httpd_req_get_url_query_str(req, buf, sizeof(buf)) == ESP_OK) {
        char param[32];
        if (httpd_query_key_value(buf, "code", param, sizeof(param)) == ESP_OK) {
            code = strtoul(param, NULL, 16);
        }
    }
    if (code) {
        sendCode(&rc, code, 24);
        ESP_LOGI("WEB", "Sent RF code 0x%06X", code);
    }
    httpd_resp_send(req, "OK", 2);
    return ESP_OK;
}

static esp_err_t status_handler(httpd_req_t *req) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "running");
    char *response = cJSON_Print(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, response, strlen(response));
    free(response);
    cJSON_Delete(root);
    return ESP_OK;
}

void start_web_server(void) {
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;
    if (httpd_start(&server, &config) == ESP_OK) {
        httpd_uri_t uri_root = { .uri = "/", .method = HTTP_GET, .handler = root_get_handler };
        httpd_register_uri_handler(server, &uri_root);
        httpd_uri_t uri_led = { .uri = "/led", .method = HTTP_GET, .handler = led_handler };
        httpd_register_uri_handler(server, &uri_led);
        httpd_uri_t uri_servo = { .uri = "/servo", .method = HTTP_GET, .handler = servo_handler };
        httpd_register_uri_handler(server, &uri_servo);
        httpd_uri_t uri_send = { .uri = "/send", .method = HTTP_GET, .handler = send_rf_handler };
        httpd_register_uri_handler(server, &uri_send);
        httpd_uri_t uri_status = { .uri = "/status", .method = HTTP_GET, .handler = status_handler };
        httpd_register_uri_handler(server, &uri_status);
        ESP_LOGI("WEB", "HTTP server started");
    }
}

// ==================== LED Blink Pattern Task (IP last two digits) ====================
void blink_digit(uint8_t digit) {
    for (int i = 0; i < digit; i++) {
        led_set(true);
        vTaskDelay(pdMS_TO_TICKS(500));
        led_set(false);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

void led_pattern_task(void *pvParameters) {
    while (1) {
        xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
        // Solid 10s
        led_set(true);
        vTaskDelay(pdMS_TO_TICKS(10000));
        // Tens digit
        if (last_ip_digit1 > 0) blink_digit(last_ip_digit1);
        else vTaskDelay(pdMS_TO_TICKS(1000));
        // Pause 4s
        vTaskDelay(pdMS_TO_TICKS(4000));
        // Units digit
        blink_digit(last_ip_digit2);
        // Solid 10s before repeat
        led_set(true);
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

// ==================== WiFi Event Handler ====================
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data) {
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        esp_wifi_connect();
        ESP_LOGI("WIFI", "Reconnecting...");
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        char ip_str[16];
        esp_ip4addr_ntoa(&event->ip_info.ip, ip_str, sizeof(ip_str));
        ESP_LOGI("WIFI", "Got IP: %s", ip_str);
        uint8_t last_octet = event->ip_info.ip.addr & 0xFF;
        last_ip_digit2 = last_octet % 10;
        last_ip_digit1 = (last_octet / 10) % 10;
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
        start_web_server();   // start web server only after IP is known
    }
}

void wifi_init(void) {
    wifi_event_group = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL);
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = WIFI_SSID,
            .password = WIFI_PASS,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI("WIFI", "WiFi started");
}

// ==================== Main ====================
void app_main(void) {
    // NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // LED init (active low)
    gpio_config_t led_conf = {
        .pin_bit_mask = (1ULL << LED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&led_conf);
    led_set(false);  // start OFF

    // Servos
    servo_init();
    servo_set_angle(1, 90);  // middle position
    servo_set_angle(2, 90);

    // 433MHz rc‑switch
    initSwich(&rc); // Initialize struct (note typo in header)
    enableTransmit(&rc, RF_TX_GPIO); // Enable transmitter
    xTaskCreate(rf_receiver_task, "rf_rx", 4096, NULL, 5, NULL);

    // WiFi & LED pattern
    wifi_init();
    xTaskCreate(led_pattern_task, "led_pattern", 4096, NULL, 3, NULL);

    ESP_LOGI("MAIN", "System ready");
}
