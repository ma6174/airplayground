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

    /* Waits for Wi-Fi, then connects to the configured receiver on its own. */
    void AutoStart();

    /* Called by the protocol stack when the receiver demands a PIN. */
    bool WaitForPin(char pin[5]);

private:
    AirplayService() = default;
    static void SessionTask(void* arg);
    static void AutoStartTask(void* arg);
    std::string ResolveHost() const;
    void RememberHost(const std::string& host) const;

    VirtualDisplay* display_ = nullptr;
    std::atomic<bool> running_{false};
    std::atomic<bool> starting_{false};
    std::string host_;
    bool legacy_ = false;
    mutable std::string last_result_ = "idle since boot";
    TaskHandle_t task_ = nullptr;

    SemaphoreHandle_t pin_semaphore_ = nullptr;
    char pending_pin_[5] = {0};
};

#endif  // AIRPLAY_SERVICE_H
