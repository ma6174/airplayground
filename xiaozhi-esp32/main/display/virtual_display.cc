#include "virtual_display.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <material_symbols.h>
#include <noto_emoji.h>

#include <cstring>
#include <memory>

#include "lvgl_font.h"
#include "lvgl_theme.h"
#include "settings.h"

#define TAG "VirtualDisplay"

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);
LV_FONT_DECLARE(BUILTIN_ICON_FONT);
LV_FONT_DECLARE(font_material_symbols_30_4);
LV_FONT_DECLARE(font_noto_emoji_30_4);
LV_FONT_DECLARE(font_noto_sans_basic_20_4);
LV_FONT_DECLARE(font_noto_sans_basic_14_1);

/* Content layer layout: the text never touches the edges, and that margin is
 * also what the font fitting below measures against. */
static constexpr int kContentMarginX = 32;
static constexpr int kContentMarginY = 20;
static constexpr int kContentLineSpace = 4;

bool ParseHexColor(const char* text, lv_color_t* color) {
    if (text == nullptr || color == nullptr) {
        return false;
    }
    if (text[0] == '#') {
        text++;
    }
    uint32_t value = 0;
    int digits = 0;
    for (; text[digits] != '\0'; digits++) {
        const char c = text[digits];
        uint32_t nibble;
        if (c >= '0' && c <= '9') {
            nibble = static_cast<uint32_t>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            nibble = static_cast<uint32_t>(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            nibble = static_cast<uint32_t>(c - 'A' + 10);
        } else {
            return false;
        }
        if (digits >= 6) {
            return false;
        }
        value = (value << 4) | nibble;
    }
    if (digits != 6) {
        return false;
    }
    *color = lv_color_hex(value);
    return true;
}

bool ParseTextSize(const char* name, DisplayTextStyle::Size* size) {
    if (name == nullptr || size == nullptr) {
        return false;
    }
    if (strcmp(name, "auto") == 0) {
        *size = DisplayTextStyle::kAuto;
    } else if (strcmp(name, "large") == 0) {
        *size = DisplayTextStyle::kLarge;
    } else if (strcmp(name, "medium") == 0) {
        *size = DisplayTextStyle::kMedium;
    } else if (strcmp(name, "small") == 0) {
        *size = DisplayTextStyle::kSmall;
    } else {
        return false;
    }
    return true;
}

VirtualDisplay::VirtualDisplay(int width, int height, int fps) : fps_(fps) {
    width_ = width;
    height_ = height;
    /* Copies of the smaller built-in faces: the theme font is the large one, and
     * the content layer shrinks to these when the text is long. */
    medium_font_ = font_noto_sans_basic_20_4;
    small_font_ = font_noto_sans_basic_14_1;
    InitializeLvgl();
    InitializeThemes();

    Settings settings("display", false);
    std::string theme_name = settings.GetString("theme", "dark");
    current_theme_ = LvglThemeManager::GetInstance().GetTheme(theme_name);
    if (current_theme_ == nullptr) {
        current_theme_ = LvglThemeManager::GetInstance().GetTheme("dark");
    }
}

VirtualDisplay::~VirtualDisplay() {
    if (framebuffer_ != nullptr) {
        heap_caps_free(framebuffer_);
        framebuffer_ = nullptr;
    }
}

void VirtualDisplay::InitializeLvgl() {
    const size_t frame_bytes = static_cast<size_t>(width_) * height_ * 2;
    framebuffer_ = static_cast<uint16_t*>(heap_caps_malloc(frame_bytes, MALLOC_CAP_SPIRAM));
    if (framebuffer_ == nullptr) {
        ESP_LOGE(TAG, "no PSRAM for the %u byte frame buffer", (unsigned)frame_bytes);
        return;
    }
    memset(framebuffer_, 0, frame_bytes);

    ESP_LOGI(TAG, "virtual display %dx%d @ %d fps, frame buffer %u bytes in PSRAM", width_, height_,
             fps_, (unsigned)frame_bytes);

    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 1;
#if CONFIG_SOC_CPU_CORES_NUM > 1
    port_cfg.task_affinity = 1;
#endif
    ESP_ERROR_CHECK(lvgl_port_init(&port_cfg));

    lvgl_port_lock(0);
    // Full-refresh mode: LVGL draws straight into our buffer, so a frame is
    // ready the moment the last flush of a refresh completes.
    display_ = lv_display_create(width_, height_);
    lv_display_set_color_format(display_, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(display_, FlushReady);
    lv_display_set_buffers(display_, framebuffer_, nullptr, frame_bytes, LV_DISPLAY_RENDER_MODE_FULL);
    lv_display_set_user_data(display_, this);
    lvgl_port_unlock();

    lvgl_ready_ = display_ != nullptr;
}

void VirtualDisplay::FlushReady(lv_display_t* display, const lv_area_t* area, uint8_t* pixels) {
    (void)area;
    (void)pixels;
    auto* self = static_cast<VirtualDisplay*>(lv_display_get_user_data(display));
    if (self != nullptr && lv_display_flush_is_last(display)) {
        self->frame_counter_.fetch_add(1, std::memory_order_relaxed);
    }
    lv_display_flush_ready(display);
}

void VirtualDisplay::InitializeThemes() {
    auto text_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_TEXT_FONT);
    auto icon_font = std::make_shared<LvglBuiltInFont>(&BUILTIN_ICON_FONT);
    auto large_icon_font = std::make_shared<LvglBuiltInFont>(&font_material_symbols_30_4);
    auto emoji_font = std::make_shared<LvglBuiltInFont>(&font_noto_emoji_30_4);

    auto* light_theme = new LvglTheme("light");
    light_theme->set_background_color(lv_color_hex(0xFFFFFF));
    light_theme->set_text_color(lv_color_hex(0x000000));
    light_theme->set_chat_background_color(lv_color_hex(0xE0E0E0));
    light_theme->set_user_bubble_color(lv_color_hex(0x00FF00));
    light_theme->set_assistant_bubble_color(lv_color_hex(0xDDDDDD));
    light_theme->set_system_bubble_color(lv_color_hex(0xFFFFFF));
    light_theme->set_system_text_color(lv_color_hex(0x000000));
    light_theme->set_border_color(lv_color_hex(0x000000));
    light_theme->set_low_battery_color(lv_color_hex(0x000000));
    light_theme->set_text_font(text_font);
    light_theme->set_icon_font(icon_font);
    light_theme->set_large_icon_font(large_icon_font);
    light_theme->set_emoji_font(emoji_font);

    auto* dark_theme = new LvglTheme("dark");
    dark_theme->set_background_color(lv_color_hex(0x101018));
    dark_theme->set_text_color(lv_color_hex(0xFFFFFF));
    dark_theme->set_chat_background_color(lv_color_hex(0x1F1F1F));
    dark_theme->set_user_bubble_color(lv_color_hex(0x00FF00));
    dark_theme->set_assistant_bubble_color(lv_color_hex(0x222222));
    dark_theme->set_system_bubble_color(lv_color_hex(0x101018));
    dark_theme->set_system_text_color(lv_color_hex(0xFFFFFF));
    dark_theme->set_border_color(lv_color_hex(0xFFFFFF));
    dark_theme->set_low_battery_color(lv_color_hex(0xFF0000));
    dark_theme->set_text_font(text_font);
    dark_theme->set_icon_font(icon_font);
    dark_theme->set_large_icon_font(large_icon_font);
    dark_theme->set_emoji_font(emoji_font);

    auto& theme_manager = LvglThemeManager::GetInstance();
    theme_manager.RegisterTheme("light", light_theme);
    theme_manager.RegisterTheme("dark", dark_theme);
}

void VirtualDisplay::LockRender() { lvgl_port_lock(0); }

void VirtualDisplay::UnlockRender() { lvgl_port_unlock(); }

bool VirtualDisplay::CopyFramebuffer(uint8_t* dst, size_t cap) const {
    const size_t bytes = static_cast<size_t>(width_) * height_ * 2;
    if (framebuffer_ == nullptr || dst == nullptr || cap < bytes) {
        return false;
    }
    auto* self = const_cast<VirtualDisplay*>(this);
    self->LockRender();
    memcpy(dst, framebuffer_, bytes);
    self->UnlockRender();
    return true;
}

void VirtualDisplay::SetTheme(Theme* theme) {
    /* Once the device is online, the assets partition supplies a cbin font and
     * SetTextFont releases the built-in one. Widget styles hold lv_font_t
     * pointers, so they must be rebound with the theme or the render task walks
     * a freed font (LcdDisplay::SetTheme does the same). */
    Display::SetTheme(theme);
    if (lvgl_ready_ && setup_ui_called_) {
        DisplayLockGuard lock(this);
        ApplyTheme();
    }
}

void VirtualDisplay::ApplyTheme() {
    if (current_theme_ == nullptr) {
        return;
    }
    auto* theme = static_cast<LvglTheme*>(current_theme_);
    lv_obj_t* screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, theme->background_color(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(screen, theme->text_color(), 0);
    if (theme->text_font() != nullptr) {
        lv_obj_set_style_text_font(screen, theme->text_font()->font(), 0);
    }
    for (auto* label : {status_label_, notification_label_, chat_message_label_}) {
        if (label == nullptr) {
            continue;
        }
        lv_obj_set_style_text_color(label, theme->text_color(), 0);
        if (theme->text_font() != nullptr) {
            lv_obj_set_style_text_font(label, theme->text_font()->font(), 0);
        }
    }
    /* Rare characters fall back to whatever the theme ended up with (the assets
     * partition replaces it with a wider face once online). */
    const lv_font_t* theme_font = theme->text_font() != nullptr ? theme->text_font()->font() : nullptr;
    medium_font_.fallback = theme_font;
    small_font_.fallback = theme_font;
    ApplyContentStyle();
    if (emoji_label_ != nullptr && theme->emoji_font() != nullptr) {
        lv_obj_set_style_text_font(emoji_label_, theme->emoji_font()->font(), 0);
    }
}

void VirtualDisplay::SetupUI() {
    if (setup_ui_called_ || !lvgl_ready_) {
        return;
    }
    Display::SetupUI();
    DisplayLockGuard lock(this);

    auto* theme = static_cast<LvglTheme*>(current_theme_);
    lv_obj_t* screen = lv_screen_active();
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen, theme->background_color(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);

    status_label_ = lv_label_create(screen);
    lv_label_set_text(status_label_, "");
    lv_obj_align(status_label_, LV_ALIGN_TOP_LEFT, 16, 12);

    notification_label_ = lv_label_create(screen);
    lv_label_set_text(notification_label_, "");
    lv_obj_align(notification_label_, LV_ALIGN_TOP_RIGHT, -16, 12);
    lv_obj_add_flag(notification_label_, LV_OBJ_FLAG_HIDDEN);

    emoji_label_ = lv_label_create(screen);
    const char* neutral = noto_emoji_get_utf8("neutral");
    lv_label_set_text(emoji_label_, neutral != nullptr ? neutral : "");
    lv_obj_align(emoji_label_, LV_ALIGN_CENTER, 0, -24);

    chat_message_label_ = lv_label_create(screen);
    lv_label_set_text(chat_message_label_, "");
    lv_obj_set_width(chat_message_label_, width_ - 40);
    lv_label_set_long_mode(chat_message_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(chat_message_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(chat_message_label_, LV_ALIGN_BOTTOM_MID, 0, -16);

    content_area_ = lv_obj_create(screen);
    lv_obj_remove_style_all(content_area_);
    lv_obj_set_size(content_area_, width_, height_);
    lv_obj_center(content_area_);
    lv_obj_remove_flag(content_area_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(content_area_, LV_OBJ_FLAG_HIDDEN);

    content_label_ = lv_label_create(content_area_);
    lv_label_set_text(content_label_, "");
    lv_obj_set_width(content_label_, width_ - 2 * kContentMarginX);
    lv_label_set_long_mode(content_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(content_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(content_label_, kContentLineSpace, 0);
    lv_obj_center(content_label_);

    ApplyTheme();
}

const lv_font_t* VirtualDisplay::SelectContentFont(const std::string& text,
                                                  DisplayTextStyle::Size size) {
    const lv_font_t* large = nullptr;
    if (current_theme_ != nullptr) {
        auto* theme = static_cast<LvglTheme*>(current_theme_);
        large = theme->text_font() != nullptr ? theme->text_font()->font() : nullptr;
    }
    if (large == nullptr) {
        large = &medium_font_;
    }
    switch (size) {
        case DisplayTextStyle::kLarge:
            return large;
        case DisplayTextStyle::kMedium:
            return &medium_font_;
        case DisplayTextStyle::kSmall:
            return &small_font_;
        case DisplayTextStyle::kAuto:
        default:
            break;
    }
    /* Auto: the largest face whose wrapped text still fits the screen. */
    const lv_font_t* candidates[] = {large, &medium_font_, &small_font_};
    for (const lv_font_t* candidate : candidates) {
        lv_point_t measured{};
        lv_txt_get_size(&measured, text.c_str(), candidate, 0, kContentLineSpace,
                        width_ - 2 * kContentMarginX, LV_TEXT_FLAG_NONE);
        if (measured.y <= height_ - 2 * kContentMarginY) {
            return candidate;
        }
    }
    return &small_font_;
}

void VirtualDisplay::ApplyContentStyle() {
    if (content_area_ == nullptr || content_label_ == nullptr) {
        return;
    }
    auto* theme = static_cast<LvglTheme*>(current_theme_);
    const lv_color_t text_color =
        text_style_.has_color ? text_style_.color : theme->text_color();
    lv_obj_set_style_text_color(content_label_, text_color, 0);
    if (content_font_ != nullptr) {
        lv_obj_set_style_text_font(content_label_, content_font_, 0);
    }
    if (text_style_.has_background) {
        lv_obj_set_style_bg_color(content_area_, text_style_.background, 0);
        lv_obj_set_style_bg_opa(content_area_, LV_OPA_COVER, 0);
    } else {
        lv_obj_set_style_bg_opa(content_area_, LV_OPA_TRANSP, 0);
    }
}

void VirtualDisplay::ShowText(const std::string& text) { ShowText(text, DisplayTextStyle()); }

void VirtualDisplay::ShowText(const std::string& text, const DisplayTextStyle& style) {
    if (!lvgl_ready_ || content_label_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(this);
    text_style_ = style;
    content_active_ = !text.empty();
    if (content_active_) {
        content_font_ = SelectContentFont(text, style.size);
        lv_label_set_text(content_label_, text.c_str());
        ApplyContentStyle();
        lv_obj_remove_flag(content_area_, LV_OBJ_FLAG_HIDDEN);
        for (auto* label : {status_label_, chat_message_label_, emoji_label_}) {
            if (label != nullptr) {
                lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
            }
        }
        return;
    }

    lv_obj_add_flag(content_area_, LV_OBJ_FLAG_HIDDEN);
    for (auto* label : {status_label_, chat_message_label_, emoji_label_}) {
        if (label != nullptr) {
            lv_obj_remove_flag(label, LV_OBJ_FLAG_HIDDEN);
        }
    }
    /* The status the state machine kept reporting while the text was up. */
    if (!hidden_status_.empty() && status_label_ != nullptr) {
        lv_label_set_text(status_label_, hidden_status_.c_str());
        hidden_status_.clear();
    }
}

void VirtualDisplay::SetStatus(const char* status) {
    if (status == nullptr) {
        return;
    }
    if (!content_active_) {
        LvglDisplay::SetStatus(status);
        return;
    }
    /* Text the assistant put on screen owns it: keep the newest state aside
     * (LvglDisplay::SetStatus would make the status label visible again) and
     * replay it when the text is cleared. */
    hidden_status_ = status;
}

void VirtualDisplay::SetEmotion(const char* emotion) {
    if (emoji_label_ == nullptr || emotion == nullptr) {
        return;
    }
    const char* utf8 = noto_emoji_get_utf8(emotion);
    if (utf8 == nullptr) {
        utf8 = material_symbols_get_utf8(emotion);
    }
    if (utf8 == nullptr) {
        ESP_LOGW(TAG, "no glyph for emotion '%s'", emotion);
        return;
    }
    DisplayLockGuard lock(this);
    lv_label_set_text(emoji_label_, utf8);
}

void VirtualDisplay::SetChatMessage(const char* role, const char* content) {
    if (chat_message_label_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(this);
    if (content == nullptr || content[0] == '\0') {
        lv_label_set_text(chat_message_label_, "");
        return;
    }
    lv_label_set_text(chat_message_label_, content);
}
