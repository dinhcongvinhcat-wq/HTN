    
#include <string.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <nvs_flash.h>
#include <esp_rmaker_core.h>
#include <esp_rmaker_standard_params.h>
#include <esp_rmaker_standard_devices.h>
#include <esp_rmaker_standard_types.h>
#include <app_network.h>
#include <app_insights.h>
#include "app_priv.h"
extern int app_driver_set_servo(bool state);

static const char *TAG = "app_main";
esp_rmaker_device_t *servo_device;
extern bool water_sensor_enable;
esp_rmaker_device_t *level_sensor_switch;
extern bool g_power_state;

/* ========================================================= */
esp_rmaker_device_t *temp_sensor_device;
esp_rmaker_device_t *ph_sensor_device;
esp_rmaker_device_t *turbidity_sensor_device;
esp_rmaker_device_t *pump_device;
esp_rmaker_device_t *drain_device;


esp_rmaker_param_t *ph_param;
esp_rmaker_param_t *turb_param;

static void report_water_quality(void)
{
    esp_rmaker_param_update_and_report(ph_param,
        esp_rmaker_float(app_get_current_ph()));
    esp_rmaker_param_update_and_report(turb_param,
        esp_rmaker_float(app_get_current_turbidity()));
}

/* SERVO */
static esp_err_t level_sensor_write_cb(const esp_rmaker_device_t *device,
                                       const esp_rmaker_param_t *param,
                                       const esp_rmaker_param_val_t val,
                                       void *priv_data,
                                       esp_rmaker_write_ctx_t *ctx)
{
    if (strcmp(esp_rmaker_param_get_name(param), ESP_RMAKER_DEF_POWER_NAME) == 0) {

        water_sensor_enable = val.val.b;

        ESP_LOGI(TAG, "Water Sensor -> %s", val.val.b ? "ON" : "OFF");
    }

    esp_rmaker_param_update(param, val);
    return ESP_OK;
}

/* BƠM */
static esp_err_t pump_write_cb(const esp_rmaker_device_t *device,
                               const esp_rmaker_param_t *param,
                               const esp_rmaker_param_val_t val,
                               void *priv_data,
                               esp_rmaker_write_ctx_t *ctx)
{
    if (strcmp(esp_rmaker_param_get_name(param), ESP_RMAKER_DEF_POWER_NAME) == 0) {

        if (val.val.b) {
            g_power_state = true;   

            control_gpio(DRAIN_GPIO, false);
            vTaskDelay(pdMS_TO_TICKS(200));
            control_gpio(PUMP_GPIO, true);

        } else {
            control_gpio(PUMP_GPIO, false);
            g_power_state = false;  
        }

        ESP_LOGI(TAG, "Pump -> %s", val.val.b ? "ON" : "OFF");
    }

    esp_rmaker_param_update(param, val);
    return ESP_OK;
}

/* XẢ */

static esp_err_t drain_write_cb(const esp_rmaker_device_t *device,
                                const esp_rmaker_param_t *param,
                                const esp_rmaker_param_val_t val,
                                void *priv_data,
                                esp_rmaker_write_ctx_t *ctx)
{
    if (strcmp(esp_rmaker_param_get_name(param), ESP_RMAKER_DEF_POWER_NAME) == 0) {

        if (val.val.b) {
            g_power_state = true;   

            control_gpio(PUMP_GPIO, false);
            vTaskDelay(pdMS_TO_TICKS(200));
            control_gpio(DRAIN_GPIO, true);

        } else {
            control_gpio(DRAIN_GPIO, false);
            g_power_state = false;  
        }

        ESP_LOGI(TAG, "Drain -> %s", val.val.b ? "ON" : "OFF");
        report_water_quality();
    }

    esp_rmaker_param_update(param, val);
    return ESP_OK;
}
static esp_err_t servo_write_cb(const esp_rmaker_device_t *device,
                                const esp_rmaker_param_t *param,
                                const esp_rmaker_param_val_t val,
                                void *priv_data,
                                esp_rmaker_write_ctx_t *ctx)
{
    if (strcmp(esp_rmaker_param_get_name(param), ESP_RMAKER_DEF_POWER_NAME) == 0) {

        app_driver_set_servo(val.val.b);

        ESP_LOGI("app_main", "Servo -> %s", val.val.b ? "ON" : "OFF");
    }

    esp_rmaker_param_update(param, val);
    return ESP_OK;
}

void app_main(void)
{
    app_driver_init();

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS loi, xoa...");
        nvs_flash_erase();
        nvs_flash_init();
    }

    app_network_init();

    esp_rmaker_config_t cfg = { .enable_time_sync = true };
    esp_rmaker_node_t *node = esp_rmaker_node_init(&cfg, "Water Monitor", "ESP32");

    /* Servo */
   servo_device = esp_rmaker_switch_device_create("Servo", NULL, false);
   esp_rmaker_device_add_cb(servo_device, servo_write_cb, NULL);  
   esp_rmaker_node_add_device(node, servo_device);

    /* Temp */
    temp_sensor_device = esp_rmaker_temp_sensor_device_create(
        "Temperature Sensor", NULL, DEFAULT_TEMPERATURE);
    esp_rmaker_node_add_device(node, temp_sensor_device);

    /* pH */
    ph_sensor_device = esp_rmaker_device_create("pH Sensor", "esp.device.sensor", NULL);
    ph_param = esp_rmaker_param_create(
        RMAKER_PARAM_PH, "esp.param.ph",
        esp_rmaker_float(DEFAULT_PH), PROP_FLAG_READ);
    esp_rmaker_device_add_param(ph_sensor_device, ph_param);
    esp_rmaker_node_add_device(node, ph_sensor_device);

    /* Turbidity */
    turbidity_sensor_device = esp_rmaker_device_create("Turbidity Sensor", "esp.device.sensor", NULL);
    turb_param = esp_rmaker_param_create(
        RMAKER_PARAM_TURBIDITY, "esp.param.turbidity",
        esp_rmaker_float(DEFAULT_NTU), PROP_FLAG_READ);
    esp_rmaker_device_add_param(turbidity_sensor_device, turb_param);
    esp_rmaker_node_add_device(node, turbidity_sensor_device);

    /* Pump */
    pump_device = esp_rmaker_switch_device_create("Water Pump", NULL, false);
    esp_rmaker_device_add_cb(pump_device, pump_write_cb, NULL);
    esp_rmaker_node_add_device(node, pump_device);

    /* Drain */
    drain_device = esp_rmaker_switch_device_create("Water Drain", NULL, false);
    esp_rmaker_device_add_cb(drain_device, drain_write_cb, NULL);
    esp_rmaker_node_add_device(node, drain_device);


    esp_rmaker_ota_enable_default();
    esp_rmaker_start();
    app_network_start(POP_TYPE_RANDOM);
    /* Water Level Sensor Switch */
level_sensor_switch = esp_rmaker_switch_device_create(
    "Water Level Sensor", NULL, true);

esp_rmaker_device_add_cb(level_sensor_switch, level_sensor_write_cb, NULL);
esp_rmaker_node_add_device(node, level_sensor_switch);

    /* LOOP */
    while (1) {

        float temp = app_get_current_temperature();
        float ph   = app_get_current_ph();
        float turb = app_get_current_turbidity();

        ESP_LOGI(TAG, "Loop: Temp=%.2fC pH=%.2f Turb=%.2fNTU", temp, ph, turb);

        esp_rmaker_param_update_and_report(
            esp_rmaker_device_get_param_by_name(
                temp_sensor_device, ESP_RMAKER_DEF_TEMPERATURE_NAME),
            esp_rmaker_float(temp));

        esp_rmaker_param_update_and_report(ph_param,   esp_rmaker_float(ph));
        esp_rmaker_param_update_and_report(turb_param, esp_rmaker_float(turb));

        display_sensor_data();

        if (g_power_state == false) {
            check_sensor_and_control();
        }

        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}
