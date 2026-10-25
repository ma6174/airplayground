#ifndef AIRPLAY_SERVICE_H
#define AIRPLAY_SERVICE_H

#include <atomic>
#include <string>
#include <vector>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

class VirtualDisplay;
struct DisplayTextStyle;

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
     * remembered in NVS and then to CONFIG_AIRPLAY_DEFAULT_HOST. A host that is
     * not an address is looked up in the discovery list by name. */
    bool Start(const std::string& host, bool legacy);
    void Stop();

    /* Receivers this device can see on the LAN, as JSON: name, address, port and
     * whether the receiver needs the legacy AirTunes path. */
    std::string DiscoverJson();

    /* Puts text on the whole screen; an empty string restores the chat UI. The
     * style picks colour, background and text size (auto shrinks long text). */
    bool ShowText(const std::string& text, const DisplayTextStyle& style);

    /* Feeds the 4-digit code a receiver asks for while pairing. Accepts what
     * speech recognition produces: "1234", "一二三四", or a sentence with the
     * digits in it. */
    bool EnterPin(const std::string& spoken);

    /* How a session is doing right after it was started. The assistant needs an
     * immediate answer, otherwise it cannot tell the user to read the code off
     * the receiver. */
    enum class StartState { kEstablished, kPinRequired, kFailed, kTimeout };
    StartState WaitForStart(int timeout_ms);
    bool pin_required() const { return pin_pending_.load(); }

    std::string StatusJson() const;
    bool running() const { return running_.load(); }

    /* True while the session mirrors the capture stream (a pushed or pulled
     * screen) rather than this device's own UI. */
    bool showing_capture() const { return capture_source_.load(); }

    /* Snapshot of the screen as RGB565 for the bench API. */
    size_t ScreenBytes() const;
    bool CopyScreen(uint8_t* dst, size_t cap);

    /* Current screen as one standalone IDR access unit (Annex B), encoded with
     * a private encoder so a running session keeps its reference state. */
    bool EncodeScreenFrame(uint8_t* dst, size_t cap, size_t* len);

    /* Takes a playback chunk to the TV while a legacy session with audio is up
     * (the audio service calls this for every chunk it plays). False means the
     * local speaker must play it. */
    bool AudioOutput(std::vector<int16_t>& pcm);
    bool audio_enabled() const { return audio_enabled_.load(); }
    /* Persists the choice and restarts a live session: the audio stream is
     * negotiated in SETUP 2, it cannot flip mid-session. */
    void SetAudioEnabled(bool enabled);

    /* Waits for Wi-Fi, then brings up the bench HTTP server, receiver discovery,
     * the capture-stream listener and Wi-Fi power-save settings. A session starts
     * only when it is asked for: which receiver to mirror to belongs to the
     * user. */
    void StartNetworkServices();

    /* Called by the protocol stack when the receiver demands a PIN. */
    bool WaitForPin(char pin[5]);

private:
    AirplayService() = default;
    static void SessionTask(void* arg);
    static void NetworkTask(void* arg);
    /* Watches for a capture client appearing or going away, and hands the
     * running session over to the source that should be on screen. */
    static void CaptureWatchTask(void* arg);
    std::string ResolveHost() const;
    bool ResolveLegacy() const;
    bool ResolveAudioEnabled() const;
    void Remember(const std::string& host, bool legacy) const;

    VirtualDisplay* display_ = nullptr;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> starting_{false};
    std::atomic<bool> audio_enabled_{true};
    /* True while the session streams a capture client instead of this device's
     * own screen. */
    std::atomic<bool> capture_source_{false};
    /* A session that ended because no pairing code arrived is not going to be
     * fixed by retrying: the code has to come from a person. */
    std::atomic<bool> pin_timeout_{false};
    /* True while the session is blocked waiting for the user to read a code. */
    std::atomic<bool> pin_pending_{false};
    std::string host_;
    uint16_t port_ = 7000;
    bool legacy_ = false;
    /* Name the session was started by, when it came from the discovery list. */
    std::string name_;
    mutable std::string last_result_ = "idle since boot";
    TaskHandle_t task_ = nullptr;

    SemaphoreHandle_t pin_semaphore_ = nullptr;
    char pending_pin_[5] = {0};
};

#endif  // AIRPLAY_SERVICE_H
