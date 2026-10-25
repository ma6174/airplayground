#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

/* JC4880P443C_I_W wired as a headless board: no panel, the "screen" is the
 * built-in VirtualDisplay that AirPlay sends to a TV. */

#define AUDIO_INPUT_SAMPLE_RATE 24000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

#define AUDIO_CODEC_I2C_SDA_PIN GPIO_NUM_7
#define AUDIO_CODEC_I2C_SCL_PIN GPIO_NUM_8
#define AUDIO_I2S_GPIO_MCLK GPIO_NUM_13
#define AUDIO_I2S_GPIO_BCLK GPIO_NUM_12
#define AUDIO_I2S_GPIO_WS GPIO_NUM_10
#define AUDIO_I2S_GPIO_DOUT GPIO_NUM_9
#define AUDIO_I2S_GPIO_DIN GPIO_NUM_48
#define AUDIO_CODEC_PA_PIN GPIO_NUM_11
/* ES8311 7-bit I2C address. */
#define AUDIO_CODEC_ES8311_ADDR 0x30

// 这块板的 BOOT 按键接 GPIO35（guition-jc4880p443 原厂配置）；GPIO0 是面板 TE 线，
// 别用。第三个物理键 SW1 接充电芯片 KEY，不接 ESP32。
#define BOOT_BUTTON_GPIO        GPIO_NUM_35

/* Frame the AirPlay sink encodes. 960x540 is 16:9 and keeps text legible on a
 * TV: at 480x270 a desktop shrinks four times before the TV scales it back up,
 * which turns any text into grey smears. The P4 hardware encoder carries this
 * size at 15 fps; the software one did not. */
#define VIRTUAL_DISPLAY_WIDTH 960
#define VIRTUAL_DISPLAY_HEIGHT 540
#define VIRTUAL_DISPLAY_FPS 15

#endif  // _BOARD_CONFIG_H_
