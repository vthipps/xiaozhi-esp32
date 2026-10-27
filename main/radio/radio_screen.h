#ifndef RADIO_SCREEN_H_
#define RADIO_SCREEN_H_

#include <string>

#include "display.h"

#ifdef HAVE_LVGL

// Full-screen radio view drawn on LVGL's top layer while a station plays:
// station name and frequency, a volume ring around the edge, and buttons to
// change station or stop. All public methods take the display lock and must
// be called from the application main task.
class RadioScreen {
public:
    static RadioScreen& GetInstance() {
        static RadioScreen instance;
        return instance;
    }

    void Show(Display* display, const std::string& station_name);
    void SetStatus(Display* display, const char* status);
    void Hide(Display* display);
    bool IsVisible() const { return visible_; }

private:
    RadioScreen() = default;

    void Create(Display* display);
    void ApplyStation(const std::string& station_name);

    static void OnPrevious(lv_event_t* e);
    static void OnNext(lv_event_t* e);
    static void OnStop(lv_event_t* e);
    static void OnVolumeChanged(lv_event_t* e);
    static void OnVolumeReleased(lv_event_t* e);

    lv_obj_t* root_ = nullptr;
    lv_obj_t* volume_arc_ = nullptr;
    lv_obj_t* volume_label_ = nullptr;
    lv_obj_t* name_label_ = nullptr;
    lv_obj_t* frequency_label_ = nullptr;
    lv_obj_t* status_label_ = nullptr;
    lv_obj_t* stop_button_ = nullptr;
    std::string station_name_;
    bool visible_ = false;
};

#endif  // HAVE_LVGL
#endif  // RADIO_SCREEN_H_
