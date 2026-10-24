#ifndef AIRPLAY_SERVICE_H
#define AIRPLAY_SERVICE_H

#include <atomic>
#include <string>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

class VirtualDisplay;

/**
 * AirPlay mirroring session driver.
 *
 * Owns the video source that reads the VirtualDisplay frame buffer, the HAP
 * identity, the mirroring task and the state the MCP tools report. One session
 * at a time; the protocol stack itself lives in firmware/components.
 */
class AirplayService {
public:
    static AirplayService& GetInstance();

    void AttachDisplay(VirtualDisplay* display);

    /* Starts a session in the background. An empty host falls back to the host
     * remembered in NVS and then to CONFIG_AIRPLAY_DEFAULT_HOST. */
    bool Start(const std::string& host, bool legacy);
    void Stop();

    /* Feeds the 4-digit code a receiver asks for while pairing. */
    bool EnterPin(const std::string& pin);

    std::string StatusJson() const;
    bool running() const { return running_.load(); }

    /* Snapshot of the screen as RGB565 for the bench API. */
    size_t ScreenBytes() const;
    bool CopyScreen(uint8_t* dst, size_t cap);

    /* Current screen as one standalone IDR access unit (Annex B), encoded with
     * a private encoder so a running session keeps its reference state. */
    bool EncodeScreenFrame(uint8_t* dst, size_t cap, size_t* len);

    /* Waits for Wi-Fi, then connects to the configured receiver on its own. */
    void AutoStart();

    /* Called by the protocol stack when the receiver demands a PIN. */
    bool WaitForPin(char pin[5]);

private:
    AirplayService() = default;
    static void SessionTask(void* arg);
    static void AutoStartTask(void* arg);
    std::string ResolveHost() const;
    bool ResolveLegacy() const;
    void Remember(const std::string& host, bool legacy) const;

    VirtualDisplay* display_ = nullptr;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> starting_{false};
    std::string host_;
    uint16_t port_ = 7000;
    bool legacy_ = false;
    mutable std::string last_result_ = "idle since boot";
    TaskHandle_t task_ = nullptr;

    SemaphoreHandle_t pin_semaphore_ = nullptr;
    char pending_pin_[5] = {0};
};

#endif  // AIRPLAY_SERVICE_H
