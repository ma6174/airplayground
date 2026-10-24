#include "wifi_board.h"

#include "airplay/airplay_service.h"
#include "application.h"
#include "audio/codecs/es8311_audio_codec.h"
#include "button.h"
#include "config.h"
#include "display/virtual_display.h"
#include "mcp_server.h"

#include <driver/i2c_master.h>
#include <esp_log.h>

#define TAG "Jc4880Tv"

/**
 * JC4880P443C_I_W with no local panel: the device renders its UI into memory
 * and an AirPlay receiver (a TV) shows it. Audio stays on the on-board ES8311.
 */
class Jc4880TvBoard : public WifiBoard {
private:
    i2c_master_bus_handle_t codec_i2c_bus_ = nullptr;
    Button boot_button_;
    VirtualDisplay* display_ = nullptr;

    void InitializeCodecI2c() {
        i2c_master_bus_config_t i2c_bus_cfg = {};
        i2c_bus_cfg.i2c_port = I2C_NUM_0;
        i2c_bus_cfg.sda_io_num = AUDIO_CODEC_I2C_SDA_PIN;
        i2c_bus_cfg.scl_io_num = AUDIO_CODEC_I2C_SCL_PIN;
        i2c_bus_cfg.clk_source = I2C_CLK_SRC_DEFAULT;
        i2c_bus_cfg.glitch_ignore_cnt = 7;
        i2c_bus_cfg.flags.enable_internal_pullup = 1;
        ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_cfg, &codec_i2c_bus_));
    }

    void InitializeDisplay() {
        display_ = new VirtualDisplay(VIRTUAL_DISPLAY_WIDTH, VIRTUAL_DISPLAY_HEIGHT,
                                      VIRTUAL_DISPLAY_FPS);
        AirplayService::GetInstance().AttachDisplay(display_);
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
    }

    void InitializeTools() {
        auto& mcp_server = McpServer::GetInstance();
        auto& airplay = AirplayService::GetInstance();

        mcp_server.AddTool(
            "self.airplay.discover",
            "List the AirPlay receivers on the local network: TVs, projectors and Macs that this "
            "device can mirror its screen to.\n"
            "Return:\n"
            "  JSON array of {name, host, port, legacy}. Pass `name` to `self.airplay.start`.",
            PropertyList(),
            [&airplay](const PropertyList&) -> ReturnValue { return airplay.DiscoverJson(); });

        mcp_server.AddTool(
            "self.airplay.start",
            "Mirror this device's screen to an AirPlay receiver such as a TV.\n"
            "Args:\n"
            "  `host`: receiver address, or a receiver name from `self.airplay.discover` "
            "  (empty = the receiver used last time)\n"
            "  `legacy`: true for old AirTunes-only receivers (e.g. some projectors); a discovered "
            "  receiver already carries this flag\n"
            "Return:\n"
            "  Whether the session was started. Use `self.airplay.status` afterwards.",
            PropertyList({
                Property("host", kPropertyTypeString, std::string("")),
                Property("legacy", kPropertyTypeBoolean, false),
            }),
            [&airplay](const PropertyList& properties) -> ReturnValue {
                auto host = properties["host"].value<std::string>();
                auto legacy = properties["legacy"].value<bool>();
                if (airplay.Start(host, legacy)) {
                    return std::string("started");
                }
                return std::string("already running, or no receiver address configured");
            });

        mcp_server.AddTool(
            "self.airplay.stop", "Stop mirroring to the TV.", PropertyList(),
            [&airplay](const PropertyList&) -> ReturnValue {
                airplay.Stop();
                return std::string("stopping");
            });

        mcp_server.AddTool(
            "self.airplay.status",
            "Report whether the screen is currently mirrored, to which receiver, and how the last "
            "session ended.",
            PropertyList(),
            [&airplay](const PropertyList&) -> ReturnValue { return airplay.StatusJson(); });

        mcp_server.AddTool(
            "self.airplay.enter_pin",
            "Enter the 4-digit code the TV displays while pairing with this device.",
            PropertyList({
                Property("code", kPropertyTypeString),
            }),
            [&airplay](const PropertyList& properties) -> ReturnValue {
                auto code = properties["code"].value<std::string>();
                return airplay.EnterPin(code) ? std::string("ok") : std::string("invalid code");
            });

        mcp_server.AddTool(
            "self.display.text",
            "Show text on the screen people are looking at (the TV this device mirrors to).\n"
            "The text takes the whole screen and stays until an empty string is passed; the chat "
            "user interface comes back then.\n"
            "Args:\n"
            "  `text`: the text to show, short enough to fit one screen\n"
            "Return:\n"
            "  Whether the screen is now showing it.",
            PropertyList({
                Property("text", kPropertyTypeString),
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                auto text = properties["text"].value<std::string>();
                if (display_ == nullptr) {
                    return std::string("no screen");
                }
                display_->ShowText(text);
                return text.empty() ? std::string("chat user interface restored") : std::string("shown");
            });
    }

public:
    Jc4880TvBoard() : boot_button_(BOOT_BUTTON_GPIO) {
        InitializeCodecI2c();
        InitializeDisplay();
        InitializeButtons();
        InitializeTools();
        /* 台架 HTTP 口由 AirplayService::AutoStart 的任务在网络就绪后拉起：
         * 板级构造函数早于 TCP/IP 栈初始化，在这里起 httpd 会断言。 */
        AirplayService::GetInstance().AutoStart();
    }

    virtual std::string GetBoardType() override { return "jc4880-tv"; }

    virtual AudioCodec* GetAudioCodec() override {
        static Es8311AudioCodec audio_codec(codec_i2c_bus_, I2C_NUM_0, AUDIO_INPUT_SAMPLE_RATE,
                                            AUDIO_OUTPUT_SAMPLE_RATE, AUDIO_I2S_GPIO_MCLK,
                                            AUDIO_I2S_GPIO_BCLK, AUDIO_I2S_GPIO_WS,
                                            AUDIO_I2S_GPIO_DOUT, AUDIO_I2S_GPIO_DIN,
                                            AUDIO_CODEC_PA_PIN, AUDIO_CODEC_ES8311_ADDR, true,
                                            false);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override { return display_; }
};

DECLARE_BOARD(Jc4880TvBoard);
