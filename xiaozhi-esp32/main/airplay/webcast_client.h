#ifndef WEBCAST_CLIENT_H
#define WEBCAST_CLIENT_H

#include <atomic>
#include <string>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

class VirtualDisplay;

#if CONFIG_IDF_TARGET_ESP32P4
#include "driver/jpeg_types.h"
#endif

/**
 * Browser pages from a webcaster server, pulled onto the virtual display.
 *
 * The server (tools/webcaster) renders pages headlessly and serves one stream:
 * JPEG frames with change detection, length-prefixed so the device never has
 * to parse HTML. Display and control are both device-initiated HTTP, so the
 * server keeps no device state and a reboot simply re-pulls the stream.
 */
class WebcastClient {
public:
    static WebcastClient& GetInstance();

    void AttachDisplay(VirtualDisplay* display);

    /* Opens a page on the server and starts (or keeps) the stream pull. */
    bool Open(const std::string& url);
    /* Closes the server page and stops pulling; the chat UI comes back. */
    void Stop();

    /* Numbered interactive elements, one per line; Click/Type take a number. */
    std::string Snapshot();
    bool Click(int ref);
    bool Type(int ref, const std::string& text);
    bool Scroll(int dy);
    bool Press(const std::string& key);

    /* webcaster "host:port", persisted in NVS so the bench can move. */
    std::string Host() const;
    void SetHost(const std::string& host);

    bool streaming() const { return running_.load(); }
    /* A stream that delivers nothing (not even keepalives) for a while is dead
     * even if the TCP connection lingers. */
    bool stalled() const;
    uint32_t frames() const { return frames_.load(); }

    std::string StatusJson() const;

private:
    WebcastClient() = default;
    static void StreamTask(void* arg);
    /* One shot HTTP request; returns the response body, "" when unreachable. */
    std::string Request(const char* method, const std::string& path,
                        const std::string& body, int timeout_ms);
    bool DecodeFrame(const uint8_t* jpeg, size_t len);
#if CONFIG_IDF_TARGET_ESP32P4
    bool EnsureDecoder();
    jpeg_decoder_handle_t decoder_ = nullptr;
    uint8_t* decode_out_ = nullptr;
    size_t decode_out_cap_ = 0;
    /* The hardware decoder DMAs straight from these buffers: plain heap memory
     * makes jpeg_decoder_process fail with ESP_ERR_INVALID_ARG. */
    uint8_t* decode_in_ = nullptr;
    size_t decode_in_cap_ = 0;
#endif

    VirtualDisplay* display_ = nullptr;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    TaskHandle_t task_ = nullptr;
    mutable std::string host_;
    std::atomic<int64_t> last_rx_ms_{0};
    std::atomic<uint32_t> frames_{0};
};

#endif  // WEBCAST_CLIENT_H
