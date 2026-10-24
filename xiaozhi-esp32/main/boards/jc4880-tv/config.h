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

#define BOOT_BUTTON_GPIO GPIO_NUM_0

/* Frame the AirPlay sink encodes. 480x270 is 16:9 and keeps text legible on a
 * TV while the software encoder keeps up on both targets. */
#define VIRTUAL_DISPLAY_WIDTH 480
#define VIRTUAL_DISPLAY_HEIGHT 270
#define VIRTUAL_DISPLAY_FPS 15

#endif  // _BOARD_CONFIG_H_
