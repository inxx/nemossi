#ifndef NEMOSSI_BOARD_PINS_H
#define NEMOSSI_BOARD_PINS_H

/* GPIO constants verified from Waveshare commit
 * 5157db7c888e476fd57f8a95020800377447478f.
 * Source paths and hardware caveats: ../../docs/hardware.md.
 * Numbers are kept SDK independent so host checks can validate them too.
 */
#define NM_LCD_WIDTH 240
#define NM_LCD_HEIGHT 240
#define NM_LCD_SPI_HZ 40000000
#define NM_LCD_SPI_MODE 3
#define NM_LCD_MOSI 39
#define NM_LCD_SCLK 38
#define NM_LCD_MISO (-1)
#define NM_LCD_CS 21
#define NM_LCD_DC 45
#define NM_LCD_RESET 40
#define NM_LCD_BACKLIGHT 46

/* One shared I2C bus: touch, ES7210, ES8311, and QMI8658. */
#define NM_I2C_PORT 0
#define NM_I2C_SDA 42
#define NM_I2C_SCL 41
#define NM_I2C_HZ 400000
#define NM_TOUCH_RESET 47
#define NM_TOUCH_INTERRUPT 48
#define NM_QMI8658_ADDRESS 0x6b
#define NM_IMU_INTERRUPT 6 /* Schematic GPIO table, not used by the app. */

#define NM_I2S_PORT 0
#define NM_I2S_MCLK 8
#define NM_I2S_BCLK 9
#define NM_I2S_LRCLK 10
#define NM_I2S_DOUT 12 /* ESP32 -> ES8311 speaker DAC */
#define NM_I2S_DIN 11  /* ES7210 microphone ADC -> ESP32 */
#define NM_AUDIO_AMP_ENABLE 7

#define NM_BUTTON_PLUS 4
#define NM_BUTTON_BOOT 0
#define NM_BUTTON_POWER 5
#define NM_BUTTON_ACTIVE_LEVEL 0

/* Reserved; the application never initializes battery/power/SD pins. */
#define NM_BATTERY_ADC 1
#define NM_BATTERY_POWER_ENABLE 2
#define NM_BATTERY_CHARGING_STATUS 3
#define NM_SD_CLK 16
#define NM_SD_CMD 15
#define NM_SD_D0 17
#define NM_SD_D1 18
#define NM_SD_D2 13
#define NM_SD_D3 14
#define NM_USB_D_MINUS 19
#define NM_USB_D_PLUS 20
#define NM_UART_TX 43
#define NM_UART_RX 44

#define NM_FLASH_BYTES (16u * 1024u * 1024u)
#define NM_PSRAM_BYTES (8u * 1024u * 1024u)

#endif
