#ifndef VIRTUAL_DISPLAY_H
#define VIRTUAL_DISPLAY_H

#include "lvgl_display.h"

#include <esp_lvgl_port.h>

#include <atomic>
#include <cstdint>
#include <string>

/**
 * Display backend that renders into memory instead of an LCD panel.
 *
 * LVGL draws the whole UI into one RGB565 frame buffer in PSRAM and nothing is
 * pushed to a panel. The AirPlay sink reads that buffer, so a board without a
 * screen can still show its UI on a TV.
 */
class VirtualDisplay : public LvglDisplay {
public:
    VirtualDisplay(int width, int height, int fps);
    ~VirtualDisplay() override;

    void SetupUI() override;
    void SetStatus(const char* status) override;
    void SetEmotion(const char* emotion) override;
    void SetChatMessage(const char* role, const char* content) override;
    void SetTheme(Theme* theme) override;
    void SetPowerSaveMode(bool on) override { (void)on; }

    /* Shows text on the whole screen, hiding the chat UI until an empty string is
     * passed back. This is how the assistant puts something on the TV that is not
     * a reply: a note, a title, a list. */
    void ShowText(const std::string& text);

    /* RGB565 frame the sink encodes, and a counter bumped per rendered frame so
     * it can skip re-encoding an unchanged screen. */
    const uint16_t* framebuffer() const { return framebuffer_; }
    uint32_t frame_counter() const { return frame_counter_.load(); }

    /* Serialises the frame buffer against the LVGL render task. Hold it while
     * converting the frame, a read without it tears mid-redraw. */
    void LockRender();
    void UnlockRender();

    /* Copies the frame currently on screen (RGB565, width*height*2 bytes). */
    bool CopyFramebuffer(uint8_t* dst, size_t cap) const;
    int fps() const { return fps_; }

protected:
    bool Lock(int timeout_ms = 0) override { return lvgl_port_lock(timeout_ms); }
    void Unlock() override { lvgl_port_unlock(); }

private:
    static void FlushReady(lv_display_t* display, const lv_area_t* area, uint8_t* pixels);
    void InitializeLvgl();
    void InitializeThemes();
    void ApplyTheme();

    uint16_t* framebuffer_ = nullptr;
    std::atomic<uint32_t> frame_counter_{0};
    int fps_ = 15;
    bool lvgl_ready_ = false;

    lv_obj_t* emoji_label_ = nullptr;
    lv_obj_t* chat_message_label_ = nullptr;
    lv_obj_t* content_label_ = nullptr;
    /* True while the content label owns the screen. State updates keep landing in
     * their labels, they are just not visible. */
    bool content_active_ = false;
    std::string hidden_status_;
};

#endif  // VIRTUAL_DISPLAY_H
