#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <esp_rmaker_core.h>
extern bool water_sensor_enable;
int app_driver_set_servo(bool state);
/* =========================================================
 *  GPIO CẢM BIẾN
 * ========================================================= */
#define PH_SENSOR_GPIO          ADC1_CHANNEL_7  // pH (Analog) chan 35
#define TURBIDITY_SENSOR_GPIO   ADC1_CHANNEL_6  // Độ đục (Analog) chan 34
#define WATER_LEVEL_GPIO        13              // Mực nước XKC-Y26-V (Digital, NPN)
#define DS18B20_GPIO            4               // DS18B20 chân DQ (OneWire)

/* ================GPIO ĐIỀU KHIỂN====================== */
#define PUMP_GPIO       27              // Relay bơm cấp nước 
#define DRAIN_GPIO      19              // Relay van xả      
#define SERVO_GPIO      2               // PWM servo (LEDC)

/* =================I2C – OLED SSD1306================= */
#define I2C_MASTER_SCL_IO   22
#define I2C_MASTER_SDA_IO   21
#define I2C_MASTER_NUM      I2C_NUM_1
#define I2C_MASTER_FREQ_HZ  100000

/* ============THỜI GIAN & GIÁ TRỊ MẶC ĐỊNH=============== */
#define SENSOR_UPDATE_PERIOD    15000   // ms 
#define REPORTING_PERIOD        60      // giây

#define DEFAULT_SWITCH_POWER    false
#define DEFAULT_TEMPERATURE     26.0f
#define DEFAULT_PH              7.0f
#define DEFAULT_NTU             0.0f
#define TURBIDITY_THRESHOLD_NTU 50.0f   // NTU ngưỡng bẩn: > 50 → xả, <= 50 → sạch

/* ============SERVO – xung PWM===================== */
#define ServoMsMin  0.06f
#define ServoMsMax  2.1f
#define ServoMsAvg  ((ServoMsMax - ServoMsMin) / 2.0f)

/* ============TÊN PARAM RAINMAKER====================== */
#define RMAKER_PARAM_PH         "pH"
#define RMAKER_PARAM_TURBIDITY  "Turbidity"

/* ======== EXTERN – thiết bị RainMaker=============== */
extern esp_rmaker_device_t *temp_sensor_device;
extern esp_rmaker_device_t *ph_sensor_device;
extern esp_rmaker_device_t *turbidity_sensor_device;
extern esp_rmaker_device_t *pump_device;
extern esp_rmaker_device_t *drain_device;
extern esp_rmaker_device_t *servo_device;
int app_driver_set_state(bool state);
extern bool g_power_state;

/* ======================= KHAI BÁO HÀM ============================ */
void    app_driver_init(void);
int     app_driver_set_state(bool state);
bool    app_driver_get_state(void);
float   app_get_current_temperature(void);
float   app_get_current_ph(void);
float   app_get_current_turbidity(void);
void    control_gpio(int gpio, bool state);
void    display_sensor_data(void);
void    check_sensor_and_control(void);
