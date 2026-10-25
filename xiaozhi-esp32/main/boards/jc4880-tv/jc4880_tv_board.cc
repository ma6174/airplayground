#include "wifi_board.h"

#include "airplay/airplay_http.h"
#include "airplay/airplay_service.h"
#include "airplay/webcast_client.h"
#include "airplay_screen_camera.h"
#include "application.h"
#include "audio/codecs/es8311_audio_codec.h"
#include "button.h"
#include "camera.h"
#include "config.h"
#include "display/virtual_display.h"
#include "mcp_server.h"

extern "C" {
#include "media_net.h"
}

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
    AirplayScreenCamera* screen_camera_ = nullptr;

    /* Created on demand and shared by GetCamera(), which is how the server
     * hands this board the cloud vision URL/token (see /vision/explain too). */
    AirplayScreenCamera* ScreenCamera() {
        if (screen_camera_ == nullptr) {
            screen_camera_ = new AirplayScreenCamera(VIRTUAL_DISPLAY_WIDTH, VIRTUAL_DISPLAY_HEIGHT);
        }
        return screen_camera_;
    }

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
        WebcastClient::GetInstance().AttachDisplay(display_);
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
        auto& webcast = WebcastClient::GetInstance();

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
            "Mirror THIS DEVICE's own screen (the assistant's chat UI, what the assistant itself "
            "draws) to the TV. Use it when the user says \"put your own screen on the TV\" or "
            "\"把你自己（小智）的画面投上去\", or to show text set with self.display.text.\n"
            "Three screens can go to the TV, each with its own tool - pick by what the user named:\n"
            "  this device's own screen -> this tool\n"
            "  the Mac the user is talking to -> that Mac's cast_mac_to_tv tool\n"
            "  any other machine (cloud server, Linux box) -> self.screen.cast_server\n"
            "Args:\n"
            "  `host`: receiver address, or a receiver name from `self.airplay.discover` "
            "  (empty = the receiver used last time)\n"
            "  `legacy`: true for old AirTunes-only receivers (e.g. some projectors); a discovered "
            "  receiver already carries this flag\n"
            "Return:\n"
            "  \"mirroring\" when the screen is on the receiver, or \"pin required\" with what to do "
            "next (ask the user for the 4-digit code the receiver shows).",
            PropertyList({
                Property("host", kPropertyTypeString, std::string("")),
                Property("legacy", kPropertyTypeBoolean, false),
            }),
            [&airplay, &webcast](const PropertyList& properties) -> ReturnValue {
                auto host = properties["host"].value<std::string>();
                auto legacy = properties["legacy"].value<bool>();
                /* This tool is "the assistant's own screen". Any capture stream
                 * that is up (pushed here or pulled from another machine) wins
                 * the source pick, so the TV would show that machine instead -
                 * let it go before mirroring this device's UI. */
                media_net_pull_stop();
                media_net_drop_client();
                webcast.Stop();
                /* A session picks its source when it starts, so a live one has
                 * to be rebuilt for the device UI to come back. */
                if (airplay.running()) {
                    airplay.Stop();
                    for (int i = 0; i < 100 && airplay.running(); i++) {
                        vTaskDelay(pdMS_TO_TICKS(100));
                    }
                }
                if (!airplay.Start(host, legacy)) {
                    return std::string("not started: a session is already running, or no receiver by that "
                                       "name (self.airplay.discover lists them)");
                }
                /* Wait for the outcome: the user has to be told right away when the
                 * receiver is showing a code, otherwise nobody reads it off. */
                switch (airplay.WaitForStart(8000)) {
                    case AirplayService::StartState::kEstablished:
                        return std::string("mirroring");
                    case AirplayService::StartState::kPinRequired:
                        return std::string("pin required: the receiver is showing a 4-digit code. Ask the "
                                           "user for it, then call self.airplay.enter_pin with those digits.");
                    case AirplayService::StartState::kFailed:
                        return std::string("session failed: ") + airplay.StatusJson();
                    case AirplayService::StartState::kTimeout:
                    default:
                        return std::string("still connecting: ") + airplay.StatusJson();
                }
            });

        mcp_server.AddTool(
            "self.airplay.stop", "Stop mirroring to the TV.", PropertyList(),
            [&airplay](const PropertyList&) -> ReturnValue {
                airplay.Stop();
                return std::string("stopping");
            });

        mcp_server.AddTool(
            "self.airplay.audio",
            "Choose where the assistant's voice, music and sounds play while mirroring: the TV "
            "speakers through AirPlay, or this device's own speaker.\n"
            "Args:\n  `enabled`: true for the TV (default), false for the device speaker\n"
            "Return:\n  the state now in effect.",
            PropertyList({Property("enabled", kPropertyTypeBoolean, true)}),
            [&airplay](const PropertyList& properties) -> ReturnValue {
                bool enabled = properties["enabled"].value<bool>();
                airplay.SetAudioEnabled(enabled);
                return std::string(enabled ? "sound goes to the TV while mirroring"
                                           : "sound stays on this device");
            });

        mcp_server.AddTool(
            "self.airplay.status",
            "Report whether the screen is currently mirrored, to which receiver, and how the last "
            "session ended.",
            PropertyList(),
            [&airplay](const PropertyList&) -> ReturnValue { return airplay.StatusJson(); });

        mcp_server.AddTool(
            "self.airplay.enter_pin",
            "Enter the 4-digit code a receiver shows while pairing (self.airplay.start reports when one "
            "is needed). Digits may be spoken in Chinese: \"一二三四\" works as well as \"1234\".\n"
            "Args:\n"
            "  `code`: what the user said, the digits are extracted from it\n"
            "Return:\n"
            "  Whether the receiver accepted it; a rejected code is reported as such, not as success.",
            PropertyList({
                Property("code", kPropertyTypeString),
            }),
            [&airplay](const PropertyList& properties) -> ReturnValue {
                auto code = properties["code"].value<std::string>();
                if (!airplay.EnterPin(code)) {
                    return std::string("not accepted: either the user did not say four digits, or no "
                                       "session is waiting for a code any more (start mirroring again)");
                }
                /* Four digits delivered says nothing about whether the receiver
                 * liked them; wait for the session to either come up or ask
                 * again, so the answer is not a guess. */
                switch (airplay.WaitForStart(8000)) {
                    case AirplayService::StartState::kEstablished:
                        return std::string("code accepted, mirroring");
                    case AirplayService::StartState::kPinRequired:
                        return std::string("code rejected: the receiver shows a new code, ask the user "
                                           "for it");
                    case AirplayService::StartState::kFailed:
                        return std::string("pairing failed: ") + airplay.StatusJson();
                    case AirplayService::StartState::kTimeout:
                    default:
                        return std::string("code sent, still connecting: ") + airplay.StatusJson();
                }
            });


        /* Distinct from self.airplay.start: that mirrors this device's own
         * screen, while the webcast tools put a browser page from the server on
         * the TV and let the user look at it and interact by voice. */
        mcp_server.AddTool(
            "self.webcast.open",
            "Show a web page on the TV: a server renders it headlessly and streams the screen "
            "here, and the device mirrors it on like any other screen content. Starts mirroring "
            "too when nothing is being mirrored yet.\n"
            "Args:\n"
            "  `url`: the page to open, e.g. \"news.ycombinator.com\" or a full https:// URL\n"
            "Return:\n"
            "  \"page on TV\" when streaming, or what went wrong. Afterwards use "
            "self.webcast.snapshot to see what can be clicked.\n"
            "Prefer asking the user which page when they just say \"open a webpage\"; known "
            "sites can be mapped directly (\"B站\" -> bilibili.com).",
            PropertyList({
                Property("url", kPropertyTypeString),
            }),
            [&airplay, &webcast](const PropertyList& properties) -> ReturnValue {
                auto url = properties["url"].value<std::string>();
                if (url.empty()) {
                    return std::string("no url given");
                }
                if (webcast.Host().empty()) {
                    return std::string("no webcaster configured; set one with the bench endpoint "
                                       "/airplay/webcast_host?host=ip:port first");
                }
                if (!webcast.Open(url)) {
                    return std::string("failed: the webcaster at " + webcast.Host() +
                                       " did not answer");
                }
                if (!airplay.running()) {
                    if (!airplay.Start("", false)) {
                        return std::string("page is streaming, but starting mirroring failed: " +
                                           airplay.StatusJson());
                    }
                    switch (airplay.WaitForStart(8000)) {
                        case AirplayService::StartState::kEstablished:
                            break;
                        case AirplayService::StartState::kPinRequired:
                            return std::string("page is streaming, but the receiver shows a 4-digit "
                                               "code; ask the user and call self.airplay.enter_pin");
                        case AirplayService::StartState::kFailed:
                            return std::string("page is streaming, but mirroring failed: " +
                                               airplay.StatusJson());
                        case AirplayService::StartState::kTimeout:
                        default:
                            return std::string("page is streaming, mirroring still connecting: " +
                                               airplay.StatusJson());
                    }
                }
                return std::string("page on TV");
            });

        mcp_server.AddTool(
            "self.screen.cast_server",
            "Put ANOTHER MACHINE's screen on the TV: a cloud server, a Linux box, a second computer. "
            "That machine runs a screen-cast service and this device pulls the stream, so it works "
            "even when that machine cannot be reached from here.\n"
            "NOT for the Mac the user is talking to (that Mac pushes its own screen with cast_mac_to_tv) "
            "and NOT for this device's own screen (self.airplay.start).\n"
            "Args: host is \"ip\" or \"ip:port\" (port defaults to 8090); leave it out to reuse "
            "the machine shown last time - ask the user for the address instead of guessing.",
            PropertyList({
                Property("host", kPropertyTypeString, std::string("")),
            }),
            [&airplay, &webcast](const PropertyList& properties) -> ReturnValue {
                auto target = properties["host"].value<std::string>();
                if (target.empty()) {
                    /* The address from the last request is remembered in NVS, so
                     * "put the server on the TV" keeps working without repeating
                     * it every time. */
                    target = media_net_pull_target();
                    if (target.empty()) {
                        return std::string("no address yet: ask the user which machine to show, "
                                           "like 192.168.1.20:8090");
                    }
                }
                char host[64] = {0};
                snprintf(host, sizeof(host), "%s", target.c_str());
                uint16_t port = 8090;
                if (char* colon = strrchr(host, ':')) {
                    const long parsed = strtol(colon + 1, nullptr, 10);
                    if (parsed > 0 && parsed < 65536) {
                        port = static_cast<uint16_t>(parsed);
                        *colon = '\0';
                    }
                }
                /* One picture at a time: a page stream left running would keep a
                 * second screen coming in behind this one. */
                webcast.Stop();
                if (!media_net_pull(host, port)) {
                    return std::string("bad address: ") + target;
                }
                for (int i = 0; i < 60 && !media_net_ready(); i++) {
                    vTaskDelay(pdMS_TO_TICKS(100));
                }
                if (!media_net_ready()) {
                    media_net_pull_stop();
                    return std::string("cannot reach ") + target +
                           ": is the screen service running there, and reachable from this network?";
                }
                /* A live session picks its source when it starts; the capture
                 * watchdog rebuilds it once this stream is up, so either way the
                 * answer is "wait a moment" rather than a second restart here. */
                if (!airplay.running() && !airplay.Start("", false)) {
                    return std::string("stream is up but mirroring failed: ") + airplay.StatusJson();
                }
                for (int i = 0; i < 120 && !airplay.showing_capture(); i++) {
                    vTaskDelay(pdMS_TO_TICKS(100));
                }
                if (airplay.showing_capture()) {
                    return std::string("the screen of ") + target + " is on the TV";
                }
                return std::string("the stream is up, the TV is still switching to it: ") +
                       airplay.StatusJson();
            });
        mcp_server.AddTool(
            "self.webcast.stop",
            "Close the web page and stop showing it on the TV; the chat screen comes back. "
            "Mirroring itself keeps running.",
            PropertyList(),
            [&webcast](const PropertyList&) -> ReturnValue {
                webcast.Stop();
                return std::string("page closed");
            });

        mcp_server.AddTool(
            "self.display.text",
            "Show text on the screen people are looking at (the TV this device mirrors to).\n"
            "The text takes the whole screen and stays until an empty string is passed; the chat "
            "user interface comes back then.\n"
            "Long text wraps; text that does not fit one screen is paged automatically, so send the"
            "whole thing instead of shortening it.\n"
            "Args:\n"
            "  `text`: the text to show. Newlines start a new line.\n"
            "  `color`: text colour as RRGGBB, e.g. \"FF8800\". Optional.\n"
            "  `background`: screen colour as RRGGBB, e.g. \"000000\". Optional.\n"
            "  `size`: auto, large, medium or small. Optional, defaults to auto.\n"
            "Return:\n"
            "  Whether the screen is now showing it.",
            PropertyList({
                Property("text", kPropertyTypeString),
                Property("color", kPropertyTypeString, ""),
                Property("background", kPropertyTypeString, ""),
                Property("size", kPropertyTypeString, "auto"),
            }),
            [this](const PropertyList& properties) -> ReturnValue {
                auto text = properties["text"].value<std::string>();
                if (display_ == nullptr) {
                    return std::string("no screen");
                }
                if (text.empty()) {
                    display_->ShowText("");
                    return std::string("chat user interface restored");
                }

                DisplayTextStyle style;
                const std::string color = properties["color"].value<std::string>();
                if (!color.empty()) {
                    if (!ParseHexColor(color.c_str(), &style.color)) {
                        return std::string("color must be six hex digits, e.g. FF8800");
                    }
                    style.has_color = true;
                }
                const std::string background = properties["background"].value<std::string>();
                if (!background.empty()) {
                    if (!ParseHexColor(background.c_str(), &style.background)) {
                        return std::string("background must be six hex digits, e.g. 000000");
                    }
                    style.has_background = true;
                }
                const std::string size = properties["size"].value<std::string>();
                if (!ParseTextSize(size.c_str(), &style.size)) {
                    return std::string("size must be auto, large, medium or small");
                }
                display_->ShowText(text, style);
                return std::string("shown");
            });

        /* Let the bench API (/vision/explain) ask the cloud vision service
         * about screenshots pushed from the computer, reusing the explain
         * URL/token the server delivers through GetCamera(). */
        AirplayHttpSetVisionExplain(
            [this](const uint8_t* jpeg, size_t jpeg_len, const std::string& question) {
                return ScreenCamera()->ExplainJpeg(jpeg, jpeg_len, question);
            });

        mcp_server.AddTool(
            "self.display.image",
            "Show a picture on the screen people are looking at (the TV this device mirrors to).\n"
            "The picture fills the screen (black bars keep the aspect ratio) and stays until something "
            "else takes the screen; an empty url brings the chat user interface back.\n"
            "Starts mirroring too when nothing is being mirrored yet, and takes the screen away from "
            "a Mac or server stream that is on it.\n"
            "Args:\n"
            "  `url`: http(s) URL of a JPEG picture: up to 4 MB, up to 2 megapixels, baseline JPEG "
            "(progressive is refused). An empty string restores the chat UI.\n"
            "  A random cat picture is https://cataas.com/cat/cute?width=960 - use it as the url when "
            "the user asks to see a cat (\"显示一只猫\", \"来张猫图\"). That server picks the format of "
            "each answer itself and sometimes sends PNG or GIF, which cannot be decoded here: call "
            "the tool again for another cat.\n"
            "Return:\n"
            "  Whether the picture is now on screen, or why not.",
            PropertyList({Property("url", kPropertyTypeString)}),
            [&airplay, &webcast](const PropertyList& properties) -> ReturnValue {
                auto url = properties["url"].value<std::string>();
                if (url.empty()) {
                    airplay.ShowImageFromUrl("", nullptr);
                    return std::string("chat user interface restored");
                }
                /* A capture or page stream wins the per-session source pick, and
                 * this picture is the most recent request: let those go so the
                 * device's own screen is what the TV shows. */
                const bool was_capture = airplay.showing_capture();
                media_net_pull_stop();
                media_net_drop_client();
                webcast.Stop();
                std::string error;
                if (!airplay.ShowImageFromUrl(url, &error)) {
                    return std::string("cannot show the picture: ") + error;
                }
                if (airplay.running()) {
                    return std::string(was_capture
                                           ? "picture is up; the TV switches over from the capture "
                                             "stream in a few seconds"
                                           : "picture shown on the TV");
                }
                if (!airplay.Start("", false)) {
                    return std::string("picture is up on this device's screen, but mirroring did not "
                                       "start: ") +
                           airplay.StatusJson();
                }
                switch (airplay.WaitForStart(8000)) {
                    case AirplayService::StartState::kEstablished:
                        return std::string("picture shown on the TV");
                    case AirplayService::StartState::kPinRequired:
                        return std::string("picture is up, but the receiver shows a 4-digit code: ask "
                                           "the user for it and call self.airplay.enter_pin");
                    case AirplayService::StartState::kFailed:
                        return std::string("picture is up, but mirroring failed: ") +
                               airplay.StatusJson();
                    case AirplayService::StartState::kTimeout:
                    default:
                        return std::string("picture is up, mirroring still connecting: ") +
                               airplay.StatusJson();
                }
            });

        mcp_server.AddTool(
            "self.airplay.photo",
            "Send a picture to an AirPlay receiver with the photo protocol: the receiver gets the "
            "original JPEG and renders it at full resolution, without a mirroring session.\n"
            "Only legacy receivers (old AirTunes boxes, some projectors) accept it; other receivers "
            "refuse and self.display.image (through mirroring) is the way.\n"
            "Args:\n"
            "  `url`: http(s) URL of a JPEG picture (up to 4 MB)\n"
            "  `host`: receiver address or name from self.airplay.discover; empty = the receiver "
            "used last time\n"
            "Return:\n"
            "  The receiver's answer. Accepting the request does not prove the picture is on "
            "screen - ask the user to check the TV.",
            PropertyList({
                Property("url", kPropertyTypeString),
                Property("host", kPropertyTypeString, std::string("")),
            }),
            [&airplay](const PropertyList& properties) -> ReturnValue {
                auto url = properties["url"].value<std::string>();
                auto host = properties["host"].value<std::string>();
                return airplay.SendPhoto(url, host);
            });
    }

public:
    Jc4880TvBoard() : boot_button_(BOOT_BUTTON_GPIO) {
        InitializeCodecI2c();
        InitializeDisplay();
        InitializeButtons();
        InitializeTools();
        /* 台架 HTTP 口、接收端发现由 AirplayService::StartNetworkServices 的任务
         * 在网络就绪后拉起：板级构造函数早于 TCP/IP 栈初始化，在这里起 httpd 会
         * 断言。投屏到哪台设备由用户决定（语音 / MCP / 台架接口），开机不自动连。 */
        AirplayService::GetInstance().StartNetworkServices();
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

    /* Exposing the screen camera keeps the server's vision capability flowing
     * (it is only delivered to Board::GetCamera); it also registers the
     * generic self.camera.take_photo, whose photo here is the screen itself. */
    virtual Camera* GetCamera() override { return ScreenCamera(); }
};

DECLARE_BOARD(Jc4880TvBoard);
