#include "radio_screen.h"

#ifdef HAVE_LVGL

#include <esp_log.h>
#include <material_symbols.h>


#include "application.h"
#include "audio_codec.h"
#include "board.h"
#include "lvgl_theme.h"
#include "radio_player.h"

namespace {
const char* TAG = "RadioScreen";

constexpr uint32_t kBackground = 0x0E1117;
constexpr uint32_t kRingTrack = 0x252A33;
constexpr uint32_t kButton = 0x2A2F3A;
constexpr uint32_t kButtonPressed = 0x3A4150;
constexpr uint32_t kMuted = 0x9CA3AF;

// Scale a label around its centre (256 = 100%).
void ScaleLabel(lv_obj_t* label, int scale) {
    lv_obj_set_style_transform_pivot_x(label, LV_PCT(50), 0);
    lv_obj_set_style_transform_pivot_y(label, LV_PCT(50), 0);
    lv_obj_set_style_transform_scale(label, scale, 0);
}

// A row with an icon (icon font) followed by text (text font). Returns the text label.
lv_obj_t* MakeIconTextRow(lv_obj_t* parent, const char* icon, const lv_font_t* icon_font,
                          const lv_font_t* text_font, lv_obj_t** row_out) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 6, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t* icon_label = lv_label_create(row);
    lv_label_set_text(icon_label, icon);
    lv_obj_set_style_text_font(icon_label, icon_font, 0);
    lv_obj_set_style_text_color(icon_label, lv_color_hex(kMuted), 0);

    lv_obj_t* text_label = lv_label_create(row);
    lv_obj_set_style_text_font(text_label, text_font, 0);
    lv_obj_set_style_text_color(text_label, lv_color_hex(kMuted), 0);
    if (row_out) {
        *row_out = row;
    }
    return text_label;
}

lv_obj_t* MakeRoundButton(lv_obj_t* parent, int size, const char* icon, const lv_font_t* font,
                          lv_event_cb_t cb) {
    lv_obj_t* button = lv_button_create(parent);
    lv_obj_set_size(button, size, size);
    lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(kButton), 0);
    lv_obj_set_style_bg_color(button, lv_color_hex(kButtonPressed), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(button, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(button, 0, 0);
    lv_obj_set_style_shadow_width(button, 0, 0);
    lv_obj_set_ext_click_area(button, 10);
    lv_obj_add_event_cb(button, cb, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, icon);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, lv_color_white(), 0);
    lv_obj_center(label);
    return button;
}
}  // namespace

void RadioScreen::Create(Display* display) {
    auto theme = static_cast<LvglTheme*>(display->GetTheme());
    const lv_font_t* text_font = theme ? theme->text_font()->font() : LV_FONT_DEFAULT;
    const lv_font_t* icon_font = theme ? theme->large_icon_font()->font() : LV_FONT_DEFAULT;
    const lv_font_t* small_icon_font = theme ? theme->icon_font()->font() : LV_FONT_DEFAULT;

    root_ = lv_obj_create(lv_layer_top());
    lv_obj_set_size(root_, LV_PCT(100), LV_PCT(100));
    lv_obj_center(root_);
    lv_obj_set_style_bg_color(root_, lv_color_hex(kBackground), 0);
    lv_obj_set_style_bg_opa(root_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(root_, 0, 0);
    lv_obj_set_style_radius(root_, 0, 0);
    lv_obj_set_style_pad_all(root_, 0, 0);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(root_, LV_OBJ_FLAG_CLICKABLE);  // swallow taps meant for the screen below

    // Volume ring around the edge of the round display
    volume_arc_ = lv_arc_create(root_);
    lv_obj_set_size(volume_arc_, LV_PCT(94), LV_PCT(94));
    lv_obj_center(volume_arc_);
    lv_obj_add_flag(volume_arc_, LV_OBJ_FLAG_ADV_HITTEST);  // only the ring itself is touchable
    lv_arc_set_rotation(volume_arc_, 135);
    lv_arc_set_bg_angles(volume_arc_, 0, 270);
    lv_arc_set_range(volume_arc_, 0, 100);
    lv_obj_set_style_arc_width(volume_arc_, 12, LV_PART_MAIN);
    lv_obj_set_style_arc_width(volume_arc_, 12, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(volume_arc_, lv_color_hex(kRingTrack), LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(volume_arc_, true, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(volume_arc_, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_pad_all(volume_arc_, 5, LV_PART_KNOB);
    lv_obj_add_event_cb(volume_arc_, OnVolumeChanged, LV_EVENT_VALUE_CHANGED, this);
    lv_obj_add_event_cb(volume_arc_, OnVolumeReleased, LV_EVENT_RELEASED, this);

    // Small "radio" heading
    lv_obj_t* heading_row = nullptr;
    lv_obj_t* heading = MakeIconTextRow(root_, MATERIAL_SYMBOLS_MUSIC_NOTE, small_icon_font,
                                        text_font, &heading_row);
    lv_label_set_text(heading, "RADIO");
    lv_obj_align(heading_row, LV_ALIGN_TOP_MID, 0, 44);

    name_label_ = lv_label_create(root_);
    lv_obj_set_style_text_font(name_label_, text_font, 0);
    lv_obj_set_style_text_color(name_label_, lv_color_white(), 0);
    ScaleLabel(name_label_, 384);  // 1.5x
    lv_obj_align(name_label_, LV_ALIGN_TOP_MID, 0, 92);

    frequency_label_ = lv_label_create(root_);
    lv_obj_set_style_text_font(frequency_label_, text_font, 0);
    ScaleLabel(frequency_label_, 512);  // 2x
    lv_obj_align(frequency_label_, LV_ALIGN_TOP_MID, 0, 142);

    status_label_ = lv_label_create(root_);
    lv_obj_set_style_text_font(status_label_, text_font, 0);
    lv_obj_set_style_text_color(status_label_, lv_color_hex(kMuted), 0);
    lv_obj_align(status_label_, LV_ALIGN_TOP_MID, 0, 190);

    // Controls: previous / stop / next
    stop_button_ = MakeRoundButton(root_, 72, MATERIAL_SYMBOLS_STOP, icon_font, OnStop);
    lv_obj_align(stop_button_, LV_ALIGN_TOP_MID, 0, 222);
    lv_obj_t* previous = MakeRoundButton(root_, 56, MATERIAL_SYMBOLS_SKIP_PREVIOUS, icon_font,
                                         OnPrevious);
    lv_obj_align_to(previous, stop_button_, LV_ALIGN_OUT_LEFT_MID, -20, 0);
    lv_obj_t* next = MakeRoundButton(root_, 56, MATERIAL_SYMBOLS_SKIP_NEXT, icon_font, OnNext);
    lv_obj_align_to(next, stop_button_, LV_ALIGN_OUT_RIGHT_MID, 20, 0);

    // Volume value in the gap at the bottom of the ring
    lv_obj_t* volume_row = nullptr;
    volume_label_ = MakeIconTextRow(root_, MATERIAL_SYMBOLS_VOLUME_UP, small_icon_font, text_font,
                                    &volume_row);
    lv_obj_align(volume_row, LV_ALIGN_BOTTOM_MID, 0, -16);

    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
    ESP_LOGI(TAG, "Radio screen created");
}

void RadioScreen::ApplyStation(const std::string& station_name) {
    station_name_ = station_name;
    const RadioStation* station = RadioPlayer::FindStation(station_name);
    uint32_t colour = station ? station->colour : 0xF5A300;
    lv_color_t accent = lv_color_hex(colour);

    lv_label_set_text(name_label_, station_name.c_str());
    lv_label_set_text(frequency_label_, station ? station->frequency : "");
    lv_obj_set_style_text_color(frequency_label_, accent, 0);
    lv_obj_set_style_arc_color(volume_arc_, accent, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(stop_button_, accent, 0);

    int volume = Board::GetInstance().GetAudioCodec()->output_volume();
    lv_arc_set_value(volume_arc_, volume);
    lv_label_set_text_fmt(volume_label_, "%d", volume);
}

void RadioScreen::Show(Display* display, const std::string& station_name) {
    if (display == nullptr || display->GetTheme() == nullptr) {
        return;
    }
    DisplayLockGuard lock(display);
    if (root_ == nullptr) {
        Create(display);
    }
    ApplyStation(station_name);
    lv_label_set_text(status_label_, "Connecting...");
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(root_);
    visible_ = true;
}

void RadioScreen::SetStatus(Display* display, const char* status) {
    if (display == nullptr || root_ == nullptr) {
        return;
    }
    DisplayLockGuard lock(display);
    lv_label_set_text(status_label_, status);
}

void RadioScreen::Hide(Display* display) {
    if (display == nullptr || root_ == nullptr || !visible_) {
        return;
    }
    DisplayLockGuard lock(display);
    lv_obj_add_flag(root_, LV_OBJ_FLAG_HIDDEN);
    visible_ = false;
}

// ---- touch handlers (run in the LVGL task with the display lock held) ----

void RadioScreen::OnPrevious(lv_event_t* e) {
    auto& screen = RadioScreen::GetInstance();
    auto station = RadioPlayer::NextStation(screen.station_name_, -1);
    screen.ApplyStation(station->name);
    lv_label_set_text(screen.status_label_, "Tuning...");
    Application::GetInstance().RequestRadio(station->name, station->url);
}

void RadioScreen::OnNext(lv_event_t* e) {
    auto& screen = RadioScreen::GetInstance();
    auto station = RadioPlayer::NextStation(screen.station_name_, 1);
    screen.ApplyStation(station->name);
    lv_label_set_text(screen.status_label_, "Tuning...");
    Application::GetInstance().RequestRadio(station->name, station->url);
}

void RadioScreen::OnStop(lv_event_t* e) { Application::GetInstance().RequestStopRadio(); }

void RadioScreen::OnVolumeChanged(lv_event_t* e) {
    auto& screen = RadioScreen::GetInstance();
    int volume = lv_arc_get_value(screen.volume_arc_);
    lv_label_set_text_fmt(screen.volume_label_, "%d", volume);
}

void RadioScreen::OnVolumeReleased(lv_event_t* e) {
    auto& screen = RadioScreen::GetInstance();
    int volume = lv_arc_get_value(screen.volume_arc_);
    // SetOutputVolume also saves to flash, so only apply it when the finger lifts
    Board::GetInstance().GetAudioCodec()->SetOutputVolume(volume);
}

#endif  // HAVE_LVGL
