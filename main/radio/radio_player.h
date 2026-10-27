#ifndef RADIO_PLAYER_H_
#define RADIO_PLAYER_H_

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

#include "audio_service.h"

struct RadioStation {
    const char* name;
    const char* frequency;  // shown on screen, e.g. "93.5 FM"
    uint32_t colour;        // accent colour 0xRRGGBB
    const char* url;
    const char* aliases;  // comma separated, lower case
};

// Plays a live internet radio stream (MP3 or AAC over HTTP / ICY) to the
// speaker. Runs its own worker task; the stream keeps going (with automatic
// reconnects) until Stop() is called or the station stays unreachable.
class RadioPlayer {
public:
    using FinishedCallback = std::function<void(uint32_t playback_id, bool success)>;
    // Called from the worker task with "Connecting...", "Live" or "Reconnecting..."
    using StatusCallback = std::function<void(uint32_t playback_id, const char* status)>;

    explicit RadioPlayer(AudioService& audio_service);
    ~RadioPlayer();

    bool Start(const std::string& url, uint32_t playback_id, FinishedCallback finished_callback,
               StatusCallback status_callback = nullptr);
    void Stop();
    bool IsActive(uint32_t playback_id = 0) const;
    bool IsBusy() const;

    // Built-in station list and name matching
    static const std::vector<RadioStation>& Stations();
    static const RadioStation* FindStation(const std::string& spoken_name);
    // Station before/after `name` in the list (wraps around)
    static const RadioStation* NextStation(const std::string& name, int step);

private:
    AudioService& audio_service_;
    mutable std::mutex mutex_;
    std::string url_;
    FinishedCallback finished_callback_;
    StatusCallback status_callback_;
    TaskHandle_t task_handle_ = nullptr;
    uint32_t playback_id_ = 0;
    bool active_ = false;
    bool worker_running_ = false;
    bool cancelled_ = false;

    static void WorkerEntry(void* arg);
    void WorkerTask();
    bool IsCancelled(uint32_t playback_id) const;
    bool SleepUnlessCancelled(uint32_t playback_id, int ms) const;
    void ReportStatus(uint32_t playback_id, const char* status);
};

#endif  // RADIO_PLAYER_H_
