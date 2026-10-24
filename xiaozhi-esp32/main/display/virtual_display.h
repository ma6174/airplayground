#ifndef VIRTUAL_DISPLAY_H
#define VIRTUAL_DISPLAY_H

#include "lvgl_display.h"

#include <esp_lvgl_port.h>

#include <atomic>
#include <cstdint>

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
    void SetEmotion(const char* emotion) override;
    void SetChatMessage(const char* role, const char* content) override;
    void SetPowerSaveMode(bool on) override { (void)on; }

    /* RGB565 frame the sink encodes, and a counter bumped per rendered frame so
     * it can skip re-encoding an unchanged screen. */
    const uint16_t* framebuffer() const { return framebuffer_; }
    uint32_t frame_counter() const { return frame_counter_.load(); }
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
};

#endif  // VIRTUAL_DISPLAY_H
