# Multi Device 

## Build and Flash firmware

- The temperature value is changed by 0.5 every minute.
- It starts at some default value (25.0) and goes on increasing till 99.5. Then it starts reducing till it comes to 0.5. The cycle keeps repeating.
- You can check the temperature changes in the phone app.
- Lightbulb and Fan are dummy devices, but you can try setting the values from the phone app and see them reflect on the ESP32-S2 monitor.

### LED not working?

The ESP32-S2-Saola-1 board has the RGB LED connected to GPIO 18. However, a few earlier boards may have it on GPIO 17. Please use `CONFIG_WS2812_LED_GPIO` to set the appropriate value.

### Reset to Factory

Press and hold the BOOT button for more than 3 seconds to reset the board to factory defaults. You will have to provision the board again to use it.

