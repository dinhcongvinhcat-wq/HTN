#include <sdkconfig.h>
#include <freertos/FreeRTOS.h>
#include <freertos/timers.h>
#include <driver/adc.h>
#include "driver/ledc.h"
#include <driver/i2c.h>
#include "ssd1306.h"
#include <esp_log.h>
#include <math.h>
#include <driver/i2c_master.h>
#include <esp_adc/adc_oneshot.h>

/* Tắt brownout detector — relay kéo dòng lớn làm sụt áp */
#include "soc/rtc_cntl_reg.h"
#include "esp_system.h"

/* DS18B20 / 1-Wire */
#include "onewire_bus.h"
#include "ds18b20.h"

#include <esp_rmaker_core.h>
#include <esp_rmaker_standard_types.h>
#include <esp_rmaker_standard_params.h>

#include <app_reset.h>
#include "app_priv.h"

static const char *TAG = "app_driver";

/* =========================================================
 *  BIẾN TOÀN CỤC NỘI BỘ
 * ========================================================= */
static TimerHandle_t sensor_timer;

static float g_temperature     = -99.0f;
static float g_ph_value        = DEFAULT_PH;
static float g_turbidity_value = DEFAULT_NTU;
 bool  g_power_state     = false;

static ssd1306_handle_t ssd1306_dev = NULL;

/* DS18B20 */
static onewire_bus_handle_t    ow_bus  = NULL;
static ds18b20_device_handle_t ds18b20 = NULL;


void display_sensor_data(void);

/* =========================================================
 *  KHỞI TẠO DS18B20
 * ========================================================= */
static esp_err_t ds18b20_sensor_init(void)
{
    onewire_bus_config_t bus_cfg = {
        .bus_gpio_num = DS18B20_GPIO,
    };
    onewire_bus_rmt_config_t rmt_cfg = {
        .max_rx_bytes = 10,
    };

    esp_err_t err = onewire_new_bus_rmt(&bus_cfg, &rmt_cfg, &ow_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Tao 1-Wire bus that bai: %s", esp_err_to_name(err));
        return err;
    }

    ds18b20_config_t ds_cfg = {};
    err = ds18b20_new_device_from_bus(ow_bus, &ds_cfg, &ds18b20);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Khong tim thay DS18B20 tren GPIO %d", DS18B20_GPIO);
        ds18b20 = NULL;
        return err;
    }

    err = ds18b20_set_resolution(ds18b20, DS18B20_RESOLUTION_12B);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Set resolution DS18B20 loi: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "DS18B20 khoi tao OK tren GPIO %d", DS18B20_GPIO);
    return ESP_OK;
}

/* =========================================================
 *  ĐỌC NHIỆT ĐỘ DS18B20
 * ========================================================= */
static float read_ds18b20_temperature(void)
{
    if (ds18b20 == NULL) {
        ESP_LOGE(TAG, "DS18B20 chua san sang");
        return -99.0f;
    }

    esp_err_t err = ds18b20_trigger_temperature_conversion(ds18b20);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Trigger DS18B20 loi: %s", esp_err_to_name(err));
        return -99.0f;
    }

    float temp = 0.0f;
    err = ds18b20_get_temperature(ds18b20, &temp);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Doc DS18B20 loi: %s", esp_err_to_name(err));
        return -99.0f;
    }

    ESP_LOGI(TAG, "Nhiet do DS18B20: %.2f C", temp);
    return temp;
}

/* =========================================================
 *  TASK ĐỌC DS18B20
 * ========================================================= */
static void ds18b20_task(void *arg)
{
    while (1) {
        float temp = read_ds18b20_temperature();
        if (temp != -99.0f) {
            g_temperature = temp;
            esp_rmaker_param_update_and_report(
                esp_rmaker_device_get_param_by_type(
                    temp_sensor_device, ESP_RMAKER_PARAM_TEMPERATURE),
                esp_rmaker_float(g_temperature));
        } else {
            g_temperature = -99.0f;
        }
        display_sensor_data();
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

/* =========================================================
 *  CẢM BIẾN pH
 * ========================================================= */
static float read_ph_sensor(void)
{
    /* Lay trung binh 10 lan doc de giam nhieu ADC */
    float sum = 0.0f;
    for (int i = 0; i < 10; i++) {
        sum += (float)adc1_get_raw(PH_SENSOR_GPIO);
    }
    float raw     = sum / 10.0f;
    float voltage = raw * (3.3f / 4095.0f);

    /* Cong thuc PH-4502C */
    float ph = 7.0f + ((2.5f - voltage) / 0.18f) + PH_OFFSET;

    /* Gioi han khoang do hop le 0-14 */
    if (ph < 0.0f)  ph = 0.0f;
    if (ph > 14.0f) ph = 14.0f;

    ESP_LOGI(TAG, "pH raw=%.0f V=%.3fV pH=%.2f", raw, voltage, ph);
    return ph;
}

/* =========================================================
 *  CẢM BIẾN ĐỘ ĐỤC
 * ========================================================= */
static float read_turbidity_sensor(void)
{
    float sum = 0.0f;
    for (int i = 0; i < 800; i++) {
        sum += adc1_get_raw(TURBIDITY_SENSOR_GPIO) * (3.3f / 4095.0f);
    }
    float voltage = sum / 800.0f;
    float NTU;

    if (voltage < 0.36f) {
        NTU = 3000.0f;
    } else if (voltage > 1.8f) {
        NTU = 0.0f;
    } else {
        float v = voltage + 2.4f;
        NTU = (-1120.4f * v * v) + (5742.3f * v) - 4352.9f;
    }

    ESP_LOGI(TAG, "Turbidity V=%.3f NTU=%.2f", voltage, NTU);
    return NTU;
}


static bool is_water_level_high(void)
{
    int level = gpio_get_level(WATER_LEVEL_GPIO);
    bool is_full = (level == 0); /* NPN: LOW = có nước = ĐẦY */
    ESP_LOGI(TAG, "XKC-Y26-V NPN | GPIO%d=%d | Nuoc: %s",
             WATER_LEVEL_GPIO, level, is_full ? "DAY" : "THAP");
    return is_full;
}

/* =========================================================
 *  ĐIỀU KHIỂN GPIO
 * ========================================================= */
void control_gpio(int gpio, bool state)
{
    gpio_set_level(gpio, state ? 1 : 0);
    ESP_LOGI(TAG, "GPIO %d -> %s", gpio, state ? "ON" : "OFF");
}

static void disable_brownout_detector(void)
{
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
    ESP_LOGW(TAG, "Brownout detector DA TAT — dam bao phan cung on dinh!");
}

void check_sensor_and_control(void)
{
    
    if (is_water_level_high()) {
        ESP_LOGW(TAG, "Nuoc DAY! PUMP OFF -> DRAIN ON.");

        control_gpio(PUMP_GPIO,  false);
        vTaskDelay(pdMS_TO_TICKS(200));
        control_gpio(DRAIN_GPIO, true);

        esp_rmaker_raise_alert("Water FULL - draining");

        esp_rmaker_param_update_and_report(
            esp_rmaker_device_get_param_by_type(pump_device,  ESP_RMAKER_DEF_POWER_NAME),
            esp_rmaker_bool(false));
        esp_rmaker_param_update_and_report(
            esp_rmaker_device_get_param_by_type(drain_device, ESP_RMAKER_DEF_POWER_NAME),
            esp_rmaker_bool(true));

    } else {
        ESP_LOGI(TAG, "Nuoc THAP. DRAIN OFF -> PUMP ON.");

        control_gpio(DRAIN_GPIO, false);
        vTaskDelay(pdMS_TO_TICKS(200)); 

        control_gpio(PUMP_GPIO,  true);

        esp_rmaker_param_update_and_report(
            esp_rmaker_device_get_param_by_type(drain_device, ESP_RMAKER_DEF_POWER_NAME),
            esp_rmaker_bool(false));
        esp_rmaker_param_update_and_report(
            esp_rmaker_device_get_param_by_type(pump_device,  ESP_RMAKER_DEF_POWER_NAME),
            esp_rmaker_bool(true));
    }
}

/* =========================================================
 *  SERVO
 * ========================================================= */
void servoDeg0(void)
{
    int duty = (int)(100.0f * (ServoMsMin / 20.0f) * 81.91f);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    vTaskDelay(pdMS_TO_TICKS(2000));
    ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
}

void servoDeg90(void)
{
    int duty = (int)(100.0f * (ServoMsAvg / 20.0f) * 81.91f);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    vTaskDelay(pdMS_TO_TICKS(2000));
    ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
}

void servoDeg180(void)
{
    int duty = (int)(100.0f * (ServoMsMax / 20.0f) * 81.91f);
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    vTaskDelay(pdMS_TO_TICKS(2000));
    ledc_stop(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, 0);
}

static void app_indicator_set(bool state)
{
    if (state) {
        servoDeg180();
        ESP_LOGI(TAG, "Servo ON 180 do");
    } else {
        servoDeg90();
        ESP_LOGI(TAG, "Servo OFF 90 do");
    }
}

static void app_indicator_init(void)
{
    ledc_timer_config_t ledc_timer = {
        .speed_mode      = LEDC_LOW_SPEED_MODE,
        .timer_num       = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_13_BIT,
        .freq_hz         = 50,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&ledc_timer);

    ledc_channel_config_t ledc_channel = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel    = LEDC_CHANNEL_0,
        .timer_sel  = LEDC_TIMER_0,
        .intr_type  = LEDC_INTR_DISABLE,
        .gpio_num   = SERVO_GPIO,
        .duty       = 0,
        .hpoint     = 0,
    };
    ledc_channel_config(&ledc_channel);
    app_indicator_set(g_power_state);
}

static void set_power_state(bool target)
{
    g_power_state = target;
    app_indicator_set(target);
}

/* =========================================================
 *  TIMER CALLBACK — đọc pH, độ đục, kiểm tra mực nước
 * ========================================================= */
static void app_sensor_update(TimerHandle_t handle)
{
    g_ph_value        = read_ph_sensor();
    g_turbidity_value = read_turbidity_sensor();

    esp_rmaker_param_update_and_report(
        esp_rmaker_device_get_param_by_name(ph_sensor_device, RMAKER_PARAM_PH),
        esp_rmaker_float(g_ph_value));

    esp_rmaker_param_update_and_report(
        esp_rmaker_device_get_param_by_name(turbidity_sensor_device, RMAKER_PARAM_TURBIDITY),
        esp_rmaker_float(g_turbidity_value));

    /* APP OFF -> chạy theo cảm biến
       APP ON  -> chạy theo app, cảm biến chỉ đọc/hiển thị */
    if (g_power_state == false) {
        check_sensor_and_control();
    } else {
        ESP_LOGI(TAG, "APP MODE ON - cam bien khong dieu khien bom/xa");
    }

    display_sensor_data();

    ESP_LOGI(TAG, "Temp=%.2fC | pH=%.2f | Turb=%.2fNTU | Water=%s | Mode=%s",
             g_temperature, g_ph_value, g_turbidity_value,
             is_water_level_high() ? "DAY" : "THAP",
             g_power_state ? "APP" : "AUTO SENSOR");
}
/* =========================================================
 *  I2C & OLED SSD1306
 * ========================================================= */
void i2c_master_init(void)
{
    i2c_config_t conf = {
        .mode             = I2C_MODE_MASTER,
        .sda_io_num       = (gpio_num_t)I2C_MASTER_SDA_IO,
        .sda_pullup_en    = GPIO_PULLUP_ENABLE,
        .scl_io_num       = (gpio_num_t)I2C_MASTER_SCL_IO,
        .scl_pullup_en    = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_MASTER_FREQ_HZ,
        .clk_flags        = I2C_SCLK_SRC_FLAG_FOR_NOMAL,
    };
    i2c_param_config(I2C_MASTER_NUM, &conf);
    i2c_driver_install(I2C_MASTER_NUM, conf.mode, 0, 0, 0);
}

void display_sensor_data(void)
{
    if (ssd1306_dev == NULL) return;

    char buf[32];
    ssd1306_clear_screen(ssd1306_dev, 0x00);

    snprintf(buf, sizeof(buf), "Temp: %.1fC", g_temperature);
    ssd1306_draw_string(ssd1306_dev, 0, 0,  (const uint8_t *)buf, 16, 1);

    snprintf(buf, sizeof(buf), "pH: %.2f", g_ph_value);
    ssd1306_draw_string(ssd1306_dev, 0, 16, (const uint8_t *)buf, 16, 1);

    snprintf(buf, sizeof(buf), "Turb: %.0fNTU", g_turbidity_value);
    ssd1306_draw_string(ssd1306_dev, 0, 32, (const uint8_t *)buf, 16, 1);

    snprintf(buf, sizeof(buf), "Water: %s", is_water_level_high() ? "DAY" : "THAP");
    ssd1306_draw_string(ssd1306_dev, 0, 48, (const uint8_t *)buf, 16, 1);

    ssd1306_refresh_gram(ssd1306_dev);
}

/* =========================================================
 *  GETTER
 * ========================================================= */
float app_get_current_temperature(void) { return g_temperature;     }
float app_get_current_ph(void)          { return g_ph_value;        }
float app_get_current_turbidity(void)   { return g_turbidity_value; }

/* =========================================================
 *  KHỞI TẠO CẢM BIẾN
 * ========================================================= */
esp_err_t app_sensor_init(void)
{
    /* ADC */
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(PH_SENSOR_GPIO,        ADC_ATTEN_DB_12);
    adc1_config_channel_atten(TURBIDITY_SENSOR_GPIO, ADC_ATTEN_DB_12);

    /* GPIO mực nước XKC-Y26-V (NPN)
     * PULL_UP nội — khi không có nước OUT thả nổi → pull-up giữ HIGH
     * Khi có nước NPN kéo OUT xuống GND → GPIO đọc LOW (0V, an toàn)  */
    gpio_config_t water_conf = {
        .pin_bit_mask = ((uint64_t)1 << WATER_LEVEL_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&water_conf);
    ESP_LOGI(TAG, "XKC-Y26-V NPN | GPIO%d PULL_UP | LOW=day, HIGH=thap", WATER_LEVEL_GPIO);

    /* Khởi động: tắt cả hai */
    gpio_set_level(DRAIN_GPIO, 0);
    gpio_set_level(PUMP_GPIO,  0);

    g_ph_value        = DEFAULT_PH;
    g_turbidity_value = DEFAULT_NTU;

    /* Timer đọc cảm biến */
    sensor_timer = xTimerCreate("sensor_tm",
                                pdMS_TO_TICKS(SENSOR_UPDATE_PERIOD),
                                pdTRUE, NULL, app_sensor_update);
    if (!sensor_timer) return ESP_FAIL;

    xTimerStart(sensor_timer, 0);
    return ESP_OK;
}

/* =========================================================
 *  KHỞI TẠO DRIVER TỔNG
 * ========================================================= */
void app_driver_init(void)
{
    /* Tắt brownout detector ngay từ đầu — trước khi relay nào hoạt động */
    disable_brownout_detector();

    /* I2C + OLED */
    i2c_master_init();
    ssd1306_dev = ssd1306_create(I2C_MASTER_NUM, SSD1306_I2C_ADDRESS);
    ssd1306_refresh_gram(ssd1306_dev);
    ssd1306_clear_screen(ssd1306_dev, 0x00);

    /* GPIO output */
    gpio_config_t out_conf = {
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
        .pin_bit_mask = ((uint64_t)1 << SERVO_GPIO)
                      | ((uint64_t)1 << DRAIN_GPIO)
                      | ((uint64_t)1 << PUMP_GPIO),
    };
    gpio_config(&out_conf);

    app_indicator_init();

    /* DS18B20 */
    if (ds18b20_sensor_init() == ESP_OK) {
        xTaskCreate(ds18b20_task, "ds18b20_task", 4096, NULL, 5, NULL);
        ESP_LOGI(TAG, "DS18B20 task da khoi dong");
    } else {
        g_temperature = -99.0f;
        ESP_LOGE(TAG, "DS18B20 loi, hien thi -99C");
    }

    app_sensor_init();
    display_sensor_data();
}


int app_driver_set_state(bool state)
{
    g_power_state = true; 

    if (state) {
        control_gpio(DRAIN_GPIO, false);
        vTaskDelay(pdMS_TO_TICKS(200));
        control_gpio(PUMP_GPIO, true);

        esp_rmaker_param_update_and_report(
            esp_rmaker_device_get_param_by_name(pump_device, ESP_RMAKER_DEF_POWER_NAME),
            esp_rmaker_bool(true));

        esp_rmaker_param_update_and_report(
            esp_rmaker_device_get_param_by_name(drain_device, ESP_RMAKER_DEF_POWER_NAME),
            esp_rmaker_bool(false));

    } else {
        control_gpio(PUMP_GPIO, false);

        esp_rmaker_param_update_and_report(
            esp_rmaker_device_get_param_by_name(pump_device, ESP_RMAKER_DEF_POWER_NAME),
            esp_rmaker_bool(false));
    }

    return ESP_OK;
}