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
#include "lvgl_display/lvgl_image.h"
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
/* How long each page of a long text stays up. */
static constexpr uint32_t kPagePeriodUs = 4000000;

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

/* Decodes one UTF-8 sequence, U+FFFD for anything malformed. */
static size_t Utf8Next(const char* text, uint32_t* codepoint) {
    const auto* bytes = reinterpret_cast<const unsigned char*>(text);
    *codepoint = 0xfffdu;
    if (bytes[0] < 0x80) {
        *codepoint = bytes[0];
        return bytes[0] == 0 ? 0 : 1;
    }
    size_t width = 0;
    uint32_t value = 0;
    if (bytes[0] >= 0xc2 && bytes[0] <= 0xdf) {
        width = 2;
        value = bytes[0] & 0x1f;
    } else if (bytes[0] >= 0xe0 && bytes[0] <= 0xef) {
        width = 3;
        value = bytes[0] & 0x0f;
    } else if (bytes[0] >= 0xf0 && bytes[0] <= 0xf4) {
        width = 4;
        value = bytes[0] & 0x07;
    } else {
        return 1;
    }
    for (size_t i = 1; i < width; i++) {
        if (bytes[i] == 0) {
            return i;
        }
        value = (value << 6) | (bytes[i] & 0x3f);
    }
    *codepoint = value;
    return width;
}

/* True when this font itself has a glyph for every character of the text.
 *
 * The built-in CJK faces carry only the common characters, and a character they
 * miss is drawn from the fallback chain - which is a different size. That is how
 * one line ended up with two text sizes, so a face is only usable when it covers
 * the whole string.
 */
static bool FontCoversText(const lv_font_t* font, const char* text) {
    if (font == nullptr || font->get_glyph_dsc == nullptr) {
        return false;
    }
    for (const char* cursor = text; *cursor != '\0';) {
        uint32_t codepoint = 0;
        const size_t width = Utf8Next(cursor, &codepoint);
        if (width == 0) {
            break;
        }
        cursor += width;
        if (codepoint == '\n' || codepoint == '\r' || codepoint == ' ') {
            continue;
        }
        lv_font_glyph_dsc_t glyph{};
        if (!font->get_glyph_dsc(font, &glyph, codepoint, 0) || glyph.is_placeholder) {
            return false;
        }
    }
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
    small_font_ = height >= 480 ? font_noto_sans_basic_20_4 : font_noto_sans_basic_14_1;
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
    if (page_timer_ != nullptr) {
        esp_timer_stop(page_timer_);
        esp_timer_delete(page_timer_);
        page_timer_ = nullptr;
    }
    if (framebuffer_ != nullptr) {
        heap_caps_free(framebuffer_);
        framebuffer_ = nullptr;
    }
    if (webcast_buffer_ != nullptr) {
        heap_caps_free(webcast_buffer_);
        webcast_buffer_ = nullptr;
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

    /* The webcaster stream writes here and the image widget shows it; only a
     * warning when it is missing, the chat UI works without it. */
    webcast_buffer_ = static_cast<uint16_t*>(heap_caps_malloc(frame_bytes, MALLOC_CAP_SPIRAM));
    if (webcast_buffer_ == nullptr) {
        ESP_LOGW(TAG, "no PSRAM for webcast frames; page streaming disabled");
    } else {
        memset(webcast_buffer_, 0, frame_bytes);
        webcast_dsc_.header.w = static_cast<uint32_t>(width_);
        webcast_dsc_.header.h = static_cast<uint32_t>(height_);
        webcast_dsc_.header.cf = LV_COLOR_FORMAT_RGB565;
        webcast_dsc_.data = reinterpret_cast<const uint8_t*>(webcast_buffer_);
        webcast_dsc_.data_size = frame_bytes;
    }

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
    /* The frame is 960x540 here, twice the 480x270 these 14px faces were picked
     * for, so the chat UI would read as fine print. Step up to the 20px text and
     * 30px icon faces: both are already linked (content face and large-icon
     * face), so the bigger type costs no flash. */
    const bool large_frame = height_ >= 480;
    auto text_font = std::make_shared<LvglBuiltInFont>(
        large_frame ? &font_noto_sans_basic_20_4 : &BUILTIN_TEXT_FONT);
    auto icon_font = std::make_shared<LvglBuiltInFont>(
        large_frame ? &font_material_symbols_30_4 : &BUILTIN_ICON_FONT);
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

    content_image_ = lv_image_create(content_area_);
    lv_obj_add_flag(content_image_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_center(content_image_);

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
    const lv_font_t* requested = nullptr;
    switch (size) {
        case DisplayTextStyle::kLarge:
            requested = large;
            break;
        case DisplayTextStyle::kMedium:
            requested = &medium_font_;
            break;
        case DisplayTextStyle::kSmall:
            requested = &small_font_;
            break;
        case DisplayTextStyle::kAuto:
        default:
            break;
    }
    if (requested != nullptr) {
        /* An explicit size is a preference: keep the one-size-per-screen rule
         * and take the closest face that can actually draw the whole text. */
        if (FontCoversText(requested, text.c_str())) {
            return requested;
        }
        const lv_font_t* const faces[] = {large, &medium_font_, &small_font_};
        for (const lv_font_t* candidate : faces) {
            if (FontCoversText(candidate, text.c_str())) {
                return candidate;
            }
        }
        return large;
    }
    /* Auto: shrink only to a face that can draw every character at one size. A
     * character outside the built-in charset comes from the fallback chain at the
     * UI font's size, so mixing faces inside one label shows up as two text sizes
     * on one line. Text too long for the chosen face is paged, not clipped. */
    const lv_font_t* candidates[] = {large, &medium_font_, &small_font_};
    const lv_font_t* largest_covering = nullptr;
    for (const lv_font_t* candidate : candidates) {
        if (!FontCoversText(candidate, text.c_str())) {
            continue;
        }
        if (largest_covering == nullptr) {
            largest_covering = candidate;
        }
        lv_point_t measured{};
        lv_txt_get_size(&measured, text.c_str(), candidate, 0, kContentLineSpace,
                        width_ - 2 * kContentMarginX, LV_TEXT_FLAG_NONE);
        if (measured.y <= height_ - 2 * kContentMarginY) {
            return candidate;
        }
    }
    if (largest_covering != nullptr) {
        return largest_covering;
    }
    /* Nothing covers it: the UI face renders those characters itself (the display
     * can fetch missing glyphs), and every glyph then has the same size. */
    return large;
}

/* A page break is taken after a character it is natural to pause at. */
static bool IsBreakCharacter(const std::string& text, size_t end) {
    if (end == 0 || end > text.size()) {
        return false;
    }
    const char last = text[end - 1];
    if (last == '\n' || last == ' ' || last == ',' || last == '.' || last == ';' ||
        last == ':' || last == '!') {
        return true;
    }
    /* The third byte of \u3002 \u3001 \uff0c \uff1b \uff1a \uff01 \uff1f \uff1f */
    if (end < 3) {
        return false;
    }
    const auto* bytes = reinterpret_cast<const unsigned char*>(text.c_str() + end - 3);
    if (bytes[0] == 0xe3 && bytes[1] == 0x80 && (bytes[2] == 0x82 || bytes[2] == 0x81)) {
        return true;
    }
    return bytes[0] == 0xef && bytes[1] == 0xbc &&
           (bytes[2] == 0x8c || bytes[2] == 0x9b || bytes[2] == 0x9a || bytes[2] == 0x81 ||
            bytes[2] == 0x9f);
}

void VirtualDisplay::SplitPages(const std::string& text, const lv_font_t* font) {
    constexpr size_t kMaxPages = 8;
    const int32_t width = width_ - 2 * kContentMarginX;
    const int32_t height = height_ - 2 * kContentMarginY;

    size_t start = 0;
    while (start < text.size() && pages_.size() < kMaxPages) {
        const std::string rest = text.substr(start);
        lv_point_t measured{};
        lv_txt_get_size(&measured, rest.c_str(), font, 0, kContentLineSpace, width,
                        LV_TEXT_FLAG_NONE);
        if (measured.y <= height) {
            pages_.push_back(rest);
            return;
        }
        /* Longest prefix that still fits, then back off to a natural break. */
        size_t low = 1;
        size_t high = rest.size();
        while (low < high) {
            const size_t middle = low + (high - low + 1) / 2;
            const std::string candidate = rest.substr(0, middle);
            lv_txt_get_size(&measured, candidate.c_str(), font, 0, kContentLineSpace, width,
                            LV_TEXT_FLAG_NONE);
            if (measured.y <= height) {
                low = middle;
            } else {
                high = middle - 1;
            }
        }
        size_t cut = low;
        /* Never split a UTF-8 sequence. */
        while (cut > 1 && (static_cast<unsigned char>(rest[cut]) & 0xc0) == 0x80) {
            cut--;
        }
        for (size_t i = cut; i > cut / 2; i--) {
            if (IsBreakCharacter(rest, i)) {
                cut = i;
                break;
            }
        }
        if (cut == 0) {
            cut = low;
        }
        pages_.push_back(rest.substr(0, cut));
        start += cut;
    }
    if (start < text.size()) {
        ESP_LOGW(TAG, "text is longer than %u pages, the rest is dropped",
                 (unsigned)kMaxPages);
    }
}

void VirtualDisplay::ShowPage() {
    if (content_label_ == nullptr || pages_.empty() || page_index_ >= pages_.size()) {
        return;
    }
    lv_label_set_text(content_label_, pages_[page_index_].c_str());
}

void VirtualDisplay::UpdatePageTimer() {
    const bool wanted = pages_.size() > 1;
    if (!wanted) {
        if (page_timer_ != nullptr) {
            esp_timer_stop(page_timer_);
        }
        page_index_ = 0;
        return;
    }
    if (page_timer_ == nullptr) {
        const esp_timer_create_args_t args = {
            .callback = VirtualDisplay::PageTimer,
            .arg = this,
            .name = "text_pages",
        };
        if (esp_timer_create(&args, &page_timer_) != ESP_OK) {
            page_timer_ = nullptr;
            return;
        }
    }
    esp_timer_stop(page_timer_);
    esp_timer_start_periodic(page_timer_, kPagePeriodUs);
}

/* Runs on the timer task: holds the render lock while touching LVGL. */
void VirtualDisplay::PageTimer(void* arg) {
    auto* self = static_cast<VirtualDisplay*>(arg);
    if (self == nullptr || self->pages_.size() < 2) {
        return;
    }
    self->LockRender();
    self->page_index_ = (self->page_index_ + 1) % self->pages_.size();
    if (self->content_active_) {
        self->ShowPage();
    }
    self->UnlockRender();
}

void VirtualDisplay::EnterContent() {
    content_active_ = true;
    lv_obj_remove_flag(content_area_, LV_OBJ_FLAG_HIDDEN);
    for (auto* label : {status_label_, chat_message_label_, emoji_label_}) {
        if (label != nullptr) {
            lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void VirtualDisplay::ExitContent() {
    content_active_ = false;
    /* Hidden widgets are never rendered, so the pixels can go before the image
     * widget is reused. */
    lv_obj_add_flag(content_image_, LV_OBJ_FLAG_HIDDEN);
    content_image_data_.reset();
    lv_obj_add_flag(content_area_, LV_OBJ_FLAG_HIDDEN);
    for (auto* label : {status_label_, chat_message_label_, emoji_label_}) {
        if (label != nullptr) {
            lv_obj_remove_flag(label, LV_OBJ_FLAG_HIDDEN);
        }
    }
    /* A subtitle may have arrived while the content owned the screen; the face
     * only comes back when there is nothing to read. */
    const char* chat_text = chat_message_label_ ? lv_label_get_text(chat_message_label_) : "";
    if (emoji_label_ != nullptr && (chat_text == nullptr || chat_text[0] == '\0')) {
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    }
    /* The status the state machine kept reporting while the content was up. */
    if (!hidden_status_.empty() && status_label_ != nullptr) {
        lv_label_set_text(status_label_, hidden_status_.c_str());
        hidden_status_.clear();
    }
}

void VirtualDisplay::StopContentPages() {
    pages_.clear();
    page_index_ = 0;
    UpdatePageTimer();
}

void VirtualDisplay::ApplyContentStyle() {
    if (content_area_ == nullptr || content_label_ == nullptr) {
        return;
    }
    /* A picture owns the content layer's background (black letterbox) while it
     * is up; the text style must not repaint it on a theme change. */
    if (content_image_data_ != nullptr) {
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
    /* The page view sits above every widget, so hand the screen back first. */
    if (webcast_visible_) {
        ShowWebcastLocked(false);
    }
    text_style_ = style;
    if (text.empty()) {
        StopContentPages();
        ExitContent();
        return;
    }

    /* The text takes the screen back from a picture that might be up. */
    content_image_data_.reset();
    lv_obj_add_flag(content_image_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(content_label_, LV_OBJ_FLAG_HIDDEN);

    content_font_ = SelectContentFont(text, style.size);
    StopContentPages();
    const int32_t width = width_ - 2 * kContentMarginX;
    const int32_t height = height_ - 2 * kContentMarginY;
    lv_point_t measured{};
    lv_txt_get_size(&measured, text.c_str(), content_font_, 0, kContentLineSpace, width,
                    LV_TEXT_FLAG_NONE);
    if (measured.y > height) {
        SplitPages(text, content_font_);
    }
    if (pages_.empty()) {
        pages_.push_back(text);
    }
    ShowPage();
    ApplyContentStyle();
    UpdatePageTimer();
    EnterContent();
}

void VirtualDisplay::ShowImage(std::unique_ptr<LvglImage> image) {
    if (!lvgl_ready_ || content_image_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(this);
    /* The page view sits above every widget, so hand the screen back first. */
    if (webcast_visible_) {
        ShowWebcastLocked(false);
    }
    if (image == nullptr) {
        StopContentPages();
        ExitContent();
        return;
    }

    StopContentPages();
    content_image_data_ = std::move(image);
    lv_image_set_src(content_image_, content_image_data_->image_dsc());
    lv_obj_center(content_image_);
    lv_obj_remove_flag(content_image_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(content_label_, LV_OBJ_FLAG_HIDDEN);
    /* Pictures letterbox on black, whatever the theme or last text said. */
    lv_obj_set_style_bg_color(content_area_, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(content_area_, LV_OPA_COVER, 0);
    EnterContent();
}

void VirtualDisplay::ShowWebcastLocked(bool on) {
    if (webcast_buffer_ == nullptr || webcast_visible_ == on) {
        return;
    }
    webcast_visible_ = on;
    if (on) {
        if (webcast_image_ == nullptr) {
            webcast_image_ = lv_image_create(lv_screen_active());
            lv_image_set_src(webcast_image_, &webcast_dsc_);
        }
        /* The page view covers the whole screen: no chat UI underneath. */
        for (auto* label : {status_label_, notification_label_, emoji_label_,
                            chat_message_label_}) {
            if (label != nullptr) {
                lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
            }
        }
        if (content_area_ != nullptr) {
            lv_obj_add_flag(content_area_, LV_OBJ_FLAG_HIDDEN);
        }
        lv_obj_remove_flag(webcast_image_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_to_index(webcast_image_, -1);  // keep it on top of everything
    } else {
        if (webcast_image_ != nullptr) {
            lv_obj_add_flag(webcast_image_, LV_OBJ_FLAG_HIDDEN);
        }
        for (auto* label : {status_label_, notification_label_, emoji_label_,
                            chat_message_label_}) {
            if (label != nullptr) {
                lv_obj_remove_flag(label, LV_OBJ_FLAG_HIDDEN);
            }
        }
        if (content_active_ && content_area_ != nullptr) {
            lv_obj_remove_flag(content_area_, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

void VirtualDisplay::ShowWebcast(bool on) {
    if (!lvgl_ready_) {
        return;
    }
    DisplayLockGuard lock(this);
    ShowWebcastLocked(on);
}

bool VirtualDisplay::PushWebcastFrame(const uint16_t* rgb565) {
    if (!lvgl_ready_ || webcast_buffer_ == nullptr || rgb565 == nullptr) {
        return false;
    }
    DisplayLockGuard lock(this);
    memcpy(webcast_buffer_, rgb565,
           static_cast<size_t>(width_) * height_ * 2);
    ShowWebcastLocked(true);
    if (webcast_image_ != nullptr) {
        lv_obj_invalidate(webcast_image_);
    }
    return true;
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
        if (emoji_label_ != nullptr) {
            lv_obj_remove_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    lv_label_set_text(chat_message_label_, content);
    /* The face sits mid-screen where long subtitles reach; show one at a time. */
    if (emoji_label_ != nullptr) {
        lv_obj_add_flag(emoji_label_, LV_OBJ_FLAG_HIDDEN);
    }
}
