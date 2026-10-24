#ifndef VIRTUAL_DISPLAY_H
#define VIRTUAL_DISPLAY_H

#include "lvgl_display.h"

#include <esp_lvgl_port.h>

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include <esp_timer.h>

/** How the assistant wants its text to look on the TV. */
struct DisplayTextStyle {
    enum Size {
        kAuto, /* Largest size the text fits in, shrinking as it gets longer. */
        kLarge,
        kMedium,
        kSmall,
    };
    Size size = kAuto;
    bool has_color = false;
    lv_color_t color{};
    bool has_background = false;
    lv_color_t background{};
};

/** Parses "#RRGGBB"/"RRGGBB" into a colour. */
bool ParseHexColor(const char* text, lv_color_t* color);
/** Parses auto/large/medium/small; false for anything else. */
bool ParseTextSize(const char* name, DisplayTextStyle::Size* size);

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
    void ShowText(const std::string& text, const DisplayTextStyle& style);

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
    /* Applies text_style_ to the content layer: wrapped, centred, and at the
     * largest font the text fits in. */
    void ApplyContentStyle();
    /* Picks a face that can draw the whole text at one size; a face that lacks a
     * character would borrow it from the fallback at another size, which shows up
     * as two text sizes on one line. */
    const lv_font_t* SelectContentFont(const std::string& text, DisplayTextStyle::Size size);
    /* Splits text that does not fit one screen into pages, so nothing is cut off. */
    void SplitPages(const std::string& text, const lv_font_t* font);
    void ShowPage();
    void UpdatePageTimer();
    static void PageTimer(void* arg);

    uint16_t* framebuffer_ = nullptr;
    std::atomic<uint32_t> frame_counter_{0};
    int fps_ = 15;
    bool lvgl_ready_ = false;

    lv_obj_t* emoji_label_ = nullptr;
    lv_obj_t* chat_message_label_ = nullptr;
    lv_obj_t* content_area_ = nullptr;
    lv_obj_t* content_label_ = nullptr;
    /* The theme font is the largest size; these copies exist only so long text
     * can shrink instead of being cut off. Copies, not references: a fallback
     * chain has to be attached to a mutable lv_font_t. */
    lv_font_t medium_font_{};
    lv_font_t small_font_{};
    DisplayTextStyle text_style_{};
    const lv_font_t* content_font_ = nullptr;
    /* Pages of the current text: one entry unless it was too long for the screen. */
    std::vector<std::string> pages_;
    size_t page_index_ = 0;
    esp_timer_handle_t page_timer_ = nullptr;
    /* True while the content label owns the screen. State updates keep landing in
     * their labels, they are just not visible. */
    bool content_active_ = false;
    std::string hidden_status_;
};

#endif  // VIRTUAL_DISPLAY_H
