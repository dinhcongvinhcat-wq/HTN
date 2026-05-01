#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <esp_rmaker_core.h>

/* =========================================================
 *  GPIO CẢM BIẾN
 * ========================================================= */
#define PH_SENSOR_GPIO          ADC1_CHANNEL_7  // pH (Analog)
#define TURBIDITY_SENSOR_GPIO   ADC1_CHANNEL_6  // Độ đục (Analog)
#define WATER_LEVEL_GPIO        13              // Mực nước XKC-Y26-V (Digital, NPN)
#define DS18B20_GPIO            4               // DS18B20 chân DQ (OneWire)

/* =========================================================
 *  GPIO ĐIỀU KHIỂN
 *
 *  WATER_LEVEL_GPIO = 13  (cảm biến XKC-Y26-V, NPN)
 *    LOW  (0) = có nước → mức CAO → bật van xả DRAIN (GPIO 19)
 *    HIGH (1) = không nước → mức THẤP → bật bơm cấp PUMP (GPIO 27)
 * ========================================================= */
#define PUMP_GPIO       27              // Relay bơm cấp nước (bật khi nước THẤP)
#define DRAIN_GPIO      19              // Relay van xả       (bật khi nước ĐẦY)
#define SERVO_GPIO      2               // PWM servo (LEDC)

/* =========================================================
 *  I2C – OLED SSD1306
 *  SSD1306_I2C_ADDRESS (0x3C) đã định nghĩa trong ssd1306.h
 *  KHÔNG định nghĩa lại ở đây để tránh warning "redefined".
 * ========================================================= */
#define I2C_MASTER_SCL_IO   22
#define I2C_MASTER_SDA_IO   21
#define I2C_MASTER_NUM      I2C_NUM_1
#define I2C_MASTER_FREQ_HZ  100000

/* =========================================================
 *  THỜI GIAN & GIÁ TRỊ MẶC ĐỊNH
 * ========================================================= */
#define SENSOR_UPDATE_PERIOD    15000   // ms – chu kỳ timer đọc cảm biến
#define REPORTING_PERIOD        60      // giây

#define DEFAULT_SWITCH_POWER    false
#define DEFAULT_TEMPERATURE     26.0f
#define DEFAULT_PH              7.0f
#define DEFAULT_NTU             0.0f

/*
 *  HIEU CHUAN pH – Module PH-4502C
 *
 *  Buoc 1: De PH_OFFSET = 0.0f, build va nap code
 *  Buoc 2: Nhung cam bien vao dung dich buffer pH 7.0
 *  Buoc 3: Doc gia tri tren OLED hoac Serial monitor
 *  Buoc 4: Neu OLED hien thi 7.4 → PH_OFFSET = -0.4f
 *           Neu OLED hien thi 6.6 → PH_OFFSET = +0.4f
 *  Buoc 5: Build lai, kiem tra lai, chinh tiep neu can
 */
#define PH_OFFSET   0.0f   /* Thay doi gia tri nay de hieu chuan */

/* =========================================================
 *  SERVO – xung PWM
 * ========================================================= */
#define ServoMsMin  0.06f
#define ServoMsMax  2.1f
#define ServoMsAvg  ((ServoMsMax - ServoMsMin) / 2.0f)

/* =========================================================
 *  TÊN PARAM RAINMAKER
 *  Định nghĩa một lần ở đây để app_driver.c và app_main.c
 *  luôn dùng chung — tránh lỗi null pointer do gõ sai tên.
 * ========================================================= */
#define RMAKER_PARAM_PH         "pH"
#define RMAKER_PARAM_TURBIDITY  "Turbidity"

/* =========================================================
 *  EXTERN – thiết bị RainMaker (định nghĩa trong app_main.c)
 * ========================================================= */
extern esp_rmaker_device_t *temp_sensor_device;
extern esp_rmaker_device_t *ph_sensor_device;
extern esp_rmaker_device_t *turbidity_sensor_device;
extern esp_rmaker_device_t *pump_device;
extern esp_rmaker_device_t *drain_device;
extern esp_rmaker_device_t *servo_device;
int app_driver_set_state(bool state);
extern bool g_power_state;

/* =========================================================
 *  KHAI BÁO HÀM
 * ========================================================= */
void    app_driver_init(void);
int     app_driver_set_state(bool state);
bool    app_driver_get_state(void);
float   app_get_current_temperature(void);
float   app_get_current_ph(void);
float   app_get_current_turbidity(void);
void    control_gpio(int gpio, bool state);
void    display_sensor_data(void);
void    check_sensor_and_control(void);
void    DFPlay(void);