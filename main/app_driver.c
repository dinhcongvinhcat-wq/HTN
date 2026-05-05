#include <sdkconfig.h>
#include <freertos/FreeRTOS.h>
#include <freertos/timers.h>
#include <freertos/semphr.h>
#include <freertos/queue.h>
#include <driver/adc.h>
#include "driver/ledc.h"
#include <driver/i2c.h>
#include "ssd1306.h"
#include <esp_log.h>
#include <driver/i2c_master.h>
#include <esp_adc/adc_oneshot.h>
#include "soc/rtc_cntl_reg.h"
#include "esp_system.h"
#include "onewire_bus.h"
#include "ds18b20.h"
#include <esp_rmaker_core.h>
#include <esp_rmaker_standard_types.h>
#include <esp_rmaker_standard_params.h>
#include <app_reset.h>
#include "app_priv.h"
/* ── Định nghĩa nội bộ cho queue ── */
#define SENSOR_QUEUE_LENGTH  8

typedef struct {
    float temperature;
    float ph;
    float turbidity;
} sensor_data_t;

bool water_sensor_enable = true;
extern int app_driver_set_servo(bool state);

static const char *TAG = "app_driver";

/* ======= BIẾN TOÀN CỤC NỘI BỘ ========= */
static TimerHandle_t sensor_timer;

static float g_temperature     = -99.0f;
static float g_ph_value        = DEFAULT_PH;
static float g_turbidity_value = DEFAULT_NTU;
bool         g_power_state     = false;

static ssd1306_handle_t ssd1306_dev = NULL;

/* DS18B20 */
static onewire_bus_handle_t    ow_bus  = NULL;
static ds18b20_device_handle_t ds18b20 = NULL;

/* ============FREERTOS IPC HANDLES ================== */
SemaphoreHandle_t g_sensor_mutex       = NULL;  /* Mutex bảo vệ biến cảm biến  */
SemaphoreHandle_t g_ds18b20_ready_sem  = NULL;  /* Binary semaphore DS18B20     */
SemaphoreHandle_t g_dirty_count_sem    = NULL;  /* Counting semaphore độ đục    */
QueueHandle_t     g_sensor_queue       = NULL;  /* Queue truyền sensor_data_t   */

/* =======PROTOTYPE========== */
void display_sensor_data(void);

/* ==========KHỞI TẠO DS18B20========== */
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

/* ==========ĐỌC NHIỆT ĐỘ DS18B20============= */
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

/* =================TASK ĐỌC DS18B20=================== */
static void ds18b20_task(void *arg)
{
    while (1) {
        float temp = read_ds18b20_temperature();

        if (temp != -99.0f) {
           
            if (xSemaphoreTake(g_sensor_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                g_temperature = temp;
                xSemaphoreGive(g_sensor_mutex);
            } else {
                ESP_LOGW(TAG, "ds18b20_task: khong lay duoc mutex");
            }

            xSemaphoreGive(g_ds18b20_ready_sem);
            ESP_LOGD(TAG, "ds18b20_task: give binary semaphore");

            sensor_data_t snap;
            if (xSemaphoreTake(g_sensor_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                snap.temperature = g_temperature;
                snap.ph          = g_ph_value;
                snap.turbidity   = g_turbidity_value;
                xSemaphoreGive(g_sensor_mutex);
            } else {
                snap.temperature = temp;
                snap.ph          = DEFAULT_PH;
                snap.turbidity   = DEFAULT_NTU;
            }

            if (xQueueSend(g_sensor_queue, &snap, pdMS_TO_TICKS(10)) != pdTRUE) {
                ESP_LOGW(TAG, "ds18b20_task: queue day, bo qua ban tin");
            }

            /* --- Cập nhật RainMaker --- */
            esp_rmaker_param_update_and_report(
                esp_rmaker_device_get_param_by_type(
                    temp_sensor_device, ESP_RMAKER_PARAM_TEMPERATURE),
                esp_rmaker_float(temp));

        } else {
            if (xSemaphoreTake(g_sensor_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                g_temperature = -99.0f;
                xSemaphoreGive(g_sensor_mutex);
            }
        }

        display_sensor_data();
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

/* ==========CẢM BIẾN pH===============*/
static float read_ph_sensor(void)
{   
    #define SAMPLES 40
    int buffer_arr[SAMPLES];

    for (int i = 0; i < SAMPLES; i++) {
        buffer_arr[i] = adc1_get_raw(PH_SENSOR_GPIO);
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // sort
    for (int i = 0; i < SAMPLES - 1; i++) {
        for (int j = i + 1; j < SAMPLES; j++) {
            if (buffer_arr[i] > buffer_arr[j]) {
                int temp = buffer_arr[i];
                buffer_arr[i] = buffer_arr[j];
                buffer_arr[j] = temp;
            }
        }
    }

    unsigned long sum = 0;
    int count = 0;
    for (int i = 10; i < SAMPLES - 10; i++) {
        sum += buffer_arr[i];
        count++;
    }

    float avg_adc = (float)sum / count;
    float voltage = avg_adc * 3.3f / 4095.0f;

    printf("ADC: %.0f | Voltage: %.3f V\n", avg_adc, voltage);

    float ph = 7.0f + ((2.5f - voltage) / 0.18f);

    return ph;
}

/* ========== CẢM BIẾN ĐỘ ĐỤC============ */
static float read_turbidity_sensor(void)
{
    float sum = 0.0f;
    for (int i = 0; i < 50; i++) {
        sum += adc1_get_raw(TURBIDITY_SENSOR_GPIO);
        vTaskDelay(pdMS_TO_TICKS(2));
    }

    float raw     = sum / 50.0f;
    float voltage = raw * 3.3f / 4095.0f;
    float V_clean = 2.6f;
    float NTU     = (V_clean - voltage) * 1500;
    if (NTU < 0)    NTU = 0;
    if (NTU > 3000) NTU = 3000;

    ESP_LOGI(TAG, "RAW=%.0f  V=%.2f  NTU=%.2f", raw, voltage, NTU);
    return NTU;
}

/* ===========CẢM BIẾN MỰC NƯỚC XKC-Y26-V (PNP)===== */
static bool is_water_level_high(void)
{
    int  level   = gpio_get_level(WATER_LEVEL_GPIO);
    bool is_full = (level == 1);
    ESP_LOGI(TAG, "XKC-Y26-V PNP | GPIO%d=%d | Nuoc: %s",
             WATER_LEVEL_GPIO, level, is_full ? "DAY" : "THAP");
    return is_full;
}

/* ================ĐIỀU KHIỂN GPIO================== */
void control_gpio(int gpio, bool state)
{
    gpio_set_level(gpio, state ? 1 : 0);
    ESP_LOGI(TAG, "GPIO %d -> %s", gpio, state ? "ON" : "OFF");
}

static void disable_brownout_detector(void)
{
    WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0);
    ESP_LOGW(TAG, "Brownout detector DA TAT");
}

void check_sensor_and_control(void)
{
    if (water_sensor_enable == false) {
        ESP_LOGI(TAG, "Water sensor OFF");
        return;
    }

    bool water_full  = is_water_level_high();
    bool water_dirty = (g_turbidity_value > TURBIDITY_THRESHOLD_NTU);

    if (!water_full) {
        ESP_LOGI(TAG, "Nuoc THAP -> DRAIN OFF, PUMP ON.");
        control_gpio(DRAIN_GPIO, false);
        vTaskDelay(pdMS_TO_TICKS(200));
        control_gpio(PUMP_GPIO, true);

        esp_rmaker_param_update_and_report(
            esp_rmaker_device_get_param_by_type(drain_device, ESP_RMAKER_DEF_POWER_NAME),
            esp_rmaker_bool(false));
        esp_rmaker_param_update_and_report(
            esp_rmaker_device_get_param_by_type(pump_device,  ESP_RMAKER_DEF_POWER_NAME),
            esp_rmaker_bool(true));

    } else if (!water_dirty) {
        ESP_LOGI(TAG, "Nuoc DAY + SACH (%.0f NTU) -> PUMP OFF, DRAIN OFF.",
                 g_turbidity_value);
        control_gpio(PUMP_GPIO,  false);
        vTaskDelay(pdMS_TO_TICKS(200));
        control_gpio(DRAIN_GPIO, false);

        esp_rmaker_param_update_and_report(
            esp_rmaker_device_get_param_by_type(pump_device,  ESP_RMAKER_DEF_POWER_NAME),
            esp_rmaker_bool(false));
        esp_rmaker_param_update_and_report(
            esp_rmaker_device_get_param_by_type(drain_device, ESP_RMAKER_DEF_POWER_NAME),
            esp_rmaker_bool(false));

    } else {
        /* ── Nước ĐẦY + BẨN ── */
        if (xSemaphoreTake(g_dirty_count_sem, 0) == pdTRUE) {
            ESP_LOGW(TAG, "Nuoc DAY + BAN (%.0f NTU > %.0f) -> PUMP OFF, DRAIN ON. "
                          "[counting sem taken]",
                     g_turbidity_value, (float)TURBIDITY_THRESHOLD_NTU);

            control_gpio(PUMP_GPIO, false);
            vTaskDelay(pdMS_TO_TICKS(200));
            control_gpio(DRAIN_GPIO, true);

            esp_rmaker_raise_alert("Water DIRTY - draining");
            esp_rmaker_param_update_and_report(
                esp_rmaker_device_get_param_by_type(pump_device,  ESP_RMAKER_DEF_POWER_NAME),
                esp_rmaker_bool(false));
            esp_rmaker_param_update_and_report(
                esp_rmaker_device_get_param_by_type(drain_device, ESP_RMAKER_DEF_POWER_NAME),
                esp_rmaker_bool(true));
        } else {
            ESP_LOGI(TAG, "Nuoc BAN nhung counting sem = 0, cho chu ky tiep theo.");
        }
    }
}

/* ==============SERVO==================== */
void servo_set_angle(int angle)
{
    int      us   = 500 + (angle * 2000 / 180);
    uint32_t duty = (us * 8191) / 20000;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static void app_indicator_set(bool state)
{
    if (state) {
        servo_set_angle(180);
        ESP_LOGI(TAG, "Servo ON 180 do");
    } else {
        servo_set_angle(90);
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

/* ==================TIMER CALLBACK — đọc pH, độ đục, kiểm tra mực nước==============*/
static void app_sensor_update(TimerHandle_t handle)
{
    float ph_new   = read_ph_sensor();
    float turb_new = read_turbidity_sensor();

    if (xSemaphoreTake(g_sensor_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
        g_ph_value        = ph_new;
        g_turbidity_value = turb_new;
        xSemaphoreGive(g_sensor_mutex);
    } else {
        ESP_LOGW(TAG, "app_sensor_update: khong lay duoc mutex, ghi truc tiep");
        g_ph_value        = ph_new;
        g_turbidity_value = turb_new;
    }

    /* --- a) Binary Semaphore: kiểm tra nhiệt độ mới từ DS18B20 task --- */
    if (xSemaphoreTake(g_ds18b20_ready_sem, 0) == pdTRUE) {
        ESP_LOGD(TAG, "app_sensor_update: co nhiet do moi tu DS18B20 task");
    } else {
        ESP_LOGD(TAG, "app_sensor_update: chua co nhiet do moi, dung gia tri cu");
    }

    /* --- c) Counting Semaphore: ghi nhận sự kiện nước bẩn --- */
    if (turb_new > TURBIDITY_THRESHOLD_NTU) {
        if (xSemaphoreGive(g_dirty_count_sem) == pdTRUE) {
            ESP_LOGW(TAG, "Nuoc BAN %.0f NTU: give counting sem", turb_new);
        } else {
            ESP_LOGW(TAG, "Counting sem DAY (max 5), bo qua su kien ban lan nay");
        }
    }

    esp_rmaker_param_update_and_report(
        esp_rmaker_device_get_param_by_name(ph_sensor_device, RMAKER_PARAM_PH),
        esp_rmaker_float(g_ph_value));
    esp_rmaker_param_update_and_report(
        esp_rmaker_device_get_param_by_name(turbidity_sensor_device, RMAKER_PARAM_TURBIDITY),
        esp_rmaker_float(g_turbidity_value));

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

/* ==============I2C & OLED SSD1306=================== */
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

    snprintf(buf, sizeof(buf), "Water: %s", is_water_level_high() ? "DAY!" : "THAP");
    ssd1306_draw_string(ssd1306_dev, 0, 48, (const uint8_t *)buf, 16, 1);

    ssd1306_refresh_gram(ssd1306_dev);
}

/* ===============GETTER==================== */
float app_get_current_temperature(void)
{
    float val;
    if (xSemaphoreTake(g_sensor_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        val = g_temperature;
        xSemaphoreGive(g_sensor_mutex);
    } else {
        val = g_temperature;  
    }
    return val;
}

float app_get_current_ph(void)
{
    float val;
    if (xSemaphoreTake(g_sensor_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        val = g_ph_value;
        xSemaphoreGive(g_sensor_mutex);
    } else {
        val = g_ph_value;
    }
    return val;
}

float app_get_current_turbidity(void)
{
    float val;
    if (xSemaphoreTake(g_sensor_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        val = g_turbidity_value;
        xSemaphoreGive(g_sensor_mutex);
    } else {
        val = g_turbidity_value;
    }
    return val;
}

/* ================KHỞI TẠO IPC ================== */
static esp_err_t ipc_init(void)
{
    /* 1. Mutex */
    g_sensor_mutex = xSemaphoreCreateMutex();
    if (g_sensor_mutex == NULL) {
        ESP_LOGE(TAG, "Tao mutex that bai");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Mutex g_sensor_mutex OK");

    /* 2. Binary Semaphore */
    g_ds18b20_ready_sem = xSemaphoreCreateBinary();
    if (g_ds18b20_ready_sem == NULL) {
        ESP_LOGE(TAG, "Tao binary semaphore that bai");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Binary Semaphore g_ds18b20_ready_sem OK");

    /* 3. Counting Semaphore  */
    g_dirty_count_sem = xSemaphoreCreateCounting(5, 0);
    if (g_dirty_count_sem == NULL) {
        ESP_LOGE(TAG, "Tao counting semaphore that bai");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Counting Semaphore g_dirty_count_sem OK (max=5)");

    /* 4. Queue — */
    g_sensor_queue = xQueueCreate(SENSOR_QUEUE_LENGTH, sizeof(sensor_data_t));
    if (g_sensor_queue == NULL) {
        ESP_LOGE(TAG, "Tao queue that bai");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Queue g_sensor_queue OK (capacity=%d)", SENSOR_QUEUE_LENGTH);

    return ESP_OK;
}

/* ===============KHỞI TẠO CẢM BIẾN=================== */
esp_err_t app_sensor_init(void)
{
    /* ADC */
    adc1_config_width(ADC_WIDTH_BIT_12);
    adc1_config_channel_atten(PH_SENSOR_GPIO,        ADC_ATTEN_DB_12);
    adc1_config_channel_atten(TURBIDITY_SENSOR_GPIO, ADC_ATTEN_DB_12);

    /* GPIO mực nước XKC-Y26-V (PNP) */
    gpio_config_t water_conf = {
        .pin_bit_mask = ((uint64_t)1 << WATER_LEVEL_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&water_conf);
    ESP_LOGI(TAG, "XKC-Y26-V PNP | GPIO%d PULL_DOWN | HIGH=day, LOW=thap",
             WATER_LEVEL_GPIO);

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

/* ===================KHỞI TẠO DRIVER TỔNG=================== */
void app_driver_init(void)
{

    disable_brownout_detector();
    if (ipc_init() != ESP_OK) {
        ESP_LOGE(TAG, "IPC init that bai — dung lai!");
        esp_restart();
    }
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
        if (xSemaphoreTake(g_sensor_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            g_temperature = -99.0f;
            xSemaphoreGive(g_sensor_mutex);
        }
        ESP_LOGE(TAG, "DS18B20 loi, hien thi -99C");
    }

    app_sensor_init();
    display_sensor_data();
}

/* ===================API CÔNG KHAI======================== */
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

int app_driver_set_servo(bool state)
{
    if (state) {
        servo_set_angle(180);
    } else {
        servo_set_angle(90);
    }
    return ESP_OK;
}