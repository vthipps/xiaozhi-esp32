#include "radio_player.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstring>
#include <memory>

#include <esp_log.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>
#include <unistd.h>

#include "esp_ae_rate_cvt.h"
#include "esp_audio_simple_dec.h"
#include "esp_audio_simple_dec_default.h"

namespace {
const char* TAG = "RadioPlayer";

constexpr uint32_t kRadioTaskStackSize = 8192;
constexpr UBaseType_t kRadioTaskPriority = 3;
constexpr int kConnectTimeoutMs = 6000;
constexpr int kReadTimeoutMs = 1000;
constexpr int kNoDataReconnectMs = 10000;
constexpr int kMaxConsecutiveFailures = 3;
constexpr int kMaxRedirects = 4;
constexpr size_t kReadBufferSize = 2048;
constexpr int kPushChunkMs = 60;

// Default stations. Streams come from an unofficial relay used by
// tamilradios.com; update the URLs here if they ever stop working.
const std::vector<RadioStation> kStations = {
    {"Suryan FM", "http://radios.crabdance.com:8002/2",
     "suryan,surya,sooryan,soorian,suriyan,93.5,93 5"},
    {"Hello FM", "http://radios.crabdance.com:8002/3", "hello fm,hello,106.4,106 4"},
    {"Radio Mirchi", "http://radios.crabdance.com:8002/1", "mirchi,mirch,mirchy,98.3,98 3"},
};

std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

struct ParsedUrl {
    std::string host;
    std::string port = "80";
    std::string path = "/";
};

bool ParseHttpUrl(const std::string& url, ParsedUrl& out) {
    const std::string prefix = "http://";
    if (url.compare(0, prefix.size(), prefix) != 0) {
        return false;  // only plain http streams are supported
    }
    std::string rest = url.substr(prefix.size());
    auto slash = rest.find('/');
    std::string hostport = slash == std::string::npos ? rest : rest.substr(0, slash);
    out.path = slash == std::string::npos ? "/" : rest.substr(slash);
    auto colon = hostport.find(':');
    if (colon != std::string::npos) {
        out.host = hostport.substr(0, colon);
        out.port = hostport.substr(colon + 1);
    } else {
        out.host = hostport;
    }
    return !out.host.empty();
}

void SetSocketTimeout(int fd, int ms) {
    timeval tv = {.tv_sec = ms / 1000, .tv_usec = (ms % 1000) * 1000};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
}

// Opens the stream, following redirects. Handles both "HTTP/1.x 200" and the
// SHOUTcast "ICY 200 OK" status line. Returns a socket positioned at the start
// of the audio data, or -1.
int OpenStream(std::string url, std::string& content_type) {
    for (int redirect = 0; redirect <= kMaxRedirects; redirect++) {
        ParsedUrl u;
        if (!ParseHttpUrl(url, u)) {
            ESP_LOGE(TAG, "Unsupported stream URL: %s", url.c_str());
            return -1;
        }

        addrinfo hints = {};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        if (getaddrinfo(u.host.c_str(), u.port.c_str(), &hints, &res) != 0 || res == nullptr) {
            ESP_LOGE(TAG, "DNS lookup failed for %s", u.host.c_str());
            return -1;
        }
        int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
        if (fd < 0) {
            freeaddrinfo(res);
            return -1;
        }
        SetSocketTimeout(fd, kConnectTimeoutMs);
        if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
            ESP_LOGE(TAG, "Connect failed: %s:%s", u.host.c_str(), u.port.c_str());
            freeaddrinfo(res);
            close(fd);
            return -1;
        }
        freeaddrinfo(res);

        std::string req = "GET " + u.path + " HTTP/1.0\r\nHost: " + u.host +
                          "\r\nUser-Agent: XiaoZhiRadio/1.0\r\nAccept: */*\r\n"
                          "Icy-MetaData: 0\r\nConnection: close\r\n\r\n";
        if (send(fd, req.data(), req.size(), 0) != (int)req.size()) {
            close(fd);
            return -1;
        }

        // Read headers one byte at a time so no audio bytes are consumed.
        std::string header;
        char c;
        while (header.size() < 8192) {
            int n = recv(fd, &c, 1, 0);
            if (n <= 0) {
                break;
            }
            header.push_back(c);
            if (header.size() >= 4 && header.compare(header.size() - 4, 4, "\r\n\r\n") == 0) {
                break;
            }
            if (header.size() >= 2 && header.compare(header.size() - 2, 2, "\n\n") == 0) {
                break;
            }
        }
        if (header.empty()) {
            close(fd);
            return -1;
        }

        int status = 0;
        auto sp = header.find(' ');
        if (sp != std::string::npos) {
            status = atoi(header.c_str() + sp + 1);
        }
        std::string lower = ToLower(header);
        auto header_value = [&](const char* name) -> std::string {
            std::string key = std::string("\n") + name + ":";
            auto pos = lower.find(key);
            if (pos == std::string::npos) {
                return "";
            }
            pos += key.size();
            auto end = header.find_first_of("\r\n", pos);
            std::string v = header.substr(pos, end == std::string::npos ? std::string::npos
                                                                          : end - pos);
            v.erase(0, v.find_first_not_of(" \t"));
            return v;
        };

        if (status >= 300 && status < 400) {
            std::string location = header_value("location");
            close(fd);
            if (location.empty()) {
                return -1;
            }
            ESP_LOGI(TAG, "Redirect %d -> %s", status, location.c_str());
            url = location;
            continue;
        }
        if (status != 200) {
            ESP_LOGE(TAG, "Stream returned status %d", status);
            close(fd);
            return -1;
        }
        content_type = ToLower(header_value("content-type"));
        SetSocketTimeout(fd, kReadTimeoutMs);
        return fd;
    }
    return -1;
}

esp_audio_simple_dec_type_t TypeFromContentType(const std::string& ct) {
    if (ct.find("aac") != std::string::npos) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
    }
    if (ct.find("mpeg") != std::string::npos || ct.find("mp3") != std::string::npos) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    }
    if (ct.find("ogg") != std::string::npos) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_OGG;
    }
    return ESP_AUDIO_SIMPLE_DEC_TYPE_NONE;
}

esp_audio_simple_dec_type_t SniffType(const uint8_t* d, size_t n) {
    if (n >= 4 && memcmp(d, "OggS", 4) == 0) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_OGG;
    }
    if (n >= 3 && memcmp(d, "ID3", 3) == 0) {
        return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    }
    for (size_t i = 0; i + 1 < n; i++) {
        if (d[i] == 0xFF && (d[i + 1] & 0xF6) == 0xF0) {
            return ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;  // ADTS sync, layer 00
        }
        if (d[i] == 0xFF && (d[i + 1] & 0xE0) == 0xE0) {
            return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
        }
    }
    return ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
}

void RegisterDecodersOnce() {
    static bool registered = false;
    if (!registered) {
        esp_audio_simple_dec_register_default();
        registered = true;
    }
}

// Converts decoded PCM to mono at the speaker's sample rate and queues it.
class PcmSink {
public:
    PcmSink(AudioService& audio, uint32_t playback_id)
        : audio_(audio), playback_id_(playback_id), out_rate_(audio.GetOutputSampleRate()) {}
    ~PcmSink() {
        if (resampler_) {
            esp_ae_rate_cvt_close(resampler_);
        }
    }

    // Returns false when playback was reset (stop requested).
    bool Write(const uint8_t* data, size_t bytes, int sample_rate, int channels, int bits) {
        if (bits != 16 || channels < 1 || sample_rate <= 0 || out_rate_ <= 0) {
            return true;
        }
        const int16_t* pcm = reinterpret_cast<const int16_t*>(data);
        size_t frames = bytes / (2 * channels);
        mono_.resize(frames);
        for (size_t i = 0; i < frames; i++) {
            int32_t sum = 0;
            for (int ch = 0; ch < channels; ch++) {
                sum += pcm[i * channels + ch];
            }
            mono_[i] = static_cast<int16_t>(sum / channels);
        }

        if (sample_rate != out_rate_) {
            if (resampler_ == nullptr || in_rate_ != sample_rate) {
                if (resampler_) {
                    esp_ae_rate_cvt_close(resampler_);
                    resampler_ = nullptr;
                }
                esp_ae_rate_cvt_cfg_t cfg = {
                    .src_rate = (uint32_t)sample_rate,
                    .dest_rate = (uint32_t)out_rate_,
                    .channel = 1,
                    .bits_per_sample = ESP_AUDIO_BIT16,
                    .complexity = 2,
                    .perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED,
                };
                if (esp_ae_rate_cvt_open(&cfg, &resampler_) != ESP_AE_ERR_OK) {
                    resampler_ = nullptr;
                    ESP_LOGE(TAG, "Failed to create resampler %d -> %d", sample_rate, out_rate_);
                    return true;
                }
                in_rate_ = sample_rate;
                ESP_LOGI(TAG, "Resampling %d Hz -> %d Hz", sample_rate, out_rate_);
            }
            uint32_t out_max = 0;
            esp_ae_rate_cvt_get_max_out_sample_num(resampler_, mono_.size(), &out_max);
            resampled_.resize(out_max);
            uint32_t out_n = out_max;
            esp_ae_rate_cvt_process(resampler_, (esp_ae_sample_t)mono_.data(), mono_.size(),
                                    (esp_ae_sample_t)resampled_.data(), &out_n);
            pending_.insert(pending_.end(), resampled_.begin(), resampled_.begin() + out_n);
        } else {
            pending_.insert(pending_.end(), mono_.begin(), mono_.end());
        }

        const size_t chunk = static_cast<size_t>(out_rate_) * kPushChunkMs / 1000;
        while (pending_.size() >= chunk) {
            std::vector<int16_t> out(pending_.begin(), pending_.begin() + chunk);
            pending_.erase(pending_.begin(), pending_.begin() + chunk);
            if (!audio_.PushPcmToPlaybackQueue(std::move(out), playback_id_, true)) {
                return false;
            }
        }
        return true;
    }

private:
    AudioService& audio_;
    uint32_t playback_id_;
    int out_rate_;
    int in_rate_ = 0;
    esp_ae_rate_cvt_handle_t resampler_ = nullptr;
    std::vector<int16_t> mono_;
    std::vector<int16_t> resampled_;
    std::vector<int16_t> pending_;
};

}  // namespace

const std::vector<RadioStation>& RadioPlayer::Stations() { return kStations; }

const RadioStation* RadioPlayer::FindStation(const std::string& spoken_name) {
    std::string q = ToLower(spoken_name);
    if (q.empty()) {
        return &kStations.front();
    }
    for (const auto& st : kStations) {
        std::string name = ToLower(st.name);
        if (q.find(name) != std::string::npos || name.find(q) != std::string::npos) {
            return &st;
        }
    }
    for (const auto& st : kStations) {
        std::string aliases = st.aliases;
        size_t start = 0;
        while (start < aliases.size()) {
            size_t comma = aliases.find(',', start);
            std::string alias = aliases.substr(start, comma == std::string::npos
                                                          ? std::string::npos
                                                          : comma - start);
            if (!alias.empty() && q.find(alias) != std::string::npos) {
                return &st;
            }
            if (comma == std::string::npos) {
                break;
            }
            start = comma + 1;
        }
    }
    // "play Tamil radio" / "play some songs" -> first station
    for (const char* generic : {"tamil", "radio", "song", "music", "fm", "station"}) {
        if (q.find(generic) != std::string::npos) {
            return &kStations.front();
        }
    }
    return nullptr;
}

RadioPlayer::RadioPlayer(AudioService& audio_service) : audio_service_(audio_service) {}

RadioPlayer::~RadioPlayer() { Stop(); }

bool RadioPlayer::Start(const std::string& url, uint32_t playback_id,
                        FinishedCallback finished_callback) {
    if (playback_id == 0 || url.empty()) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (active_ || worker_running_) {
            return false;
        }
        url_ = url;
        playback_id_ = playback_id;
        finished_callback_ = std::move(finished_callback);
        active_ = true;
        worker_running_ = true;
        cancelled_ = false;
    }

    BaseType_t created = xTaskCreate(WorkerEntry, "radio", kRadioTaskStackSize, this,
                                     kRadioTaskPriority, &task_handle_);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create radio task");
        std::lock_guard<std::mutex> lock(mutex_);
        active_ = false;
        worker_running_ = false;
        cancelled_ = true;
        task_handle_ = nullptr;
        return false;
    }
    return true;
}

void RadioPlayer::Stop() {
    std::lock_guard<std::mutex> lock(mutex_);
    cancelled_ = true;
    active_ = false;
    finished_callback_ = nullptr;
}

bool RadioPlayer::IsActive(uint32_t playback_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_ && (playback_id == 0 || playback_id == playback_id_);
}

bool RadioPlayer::IsBusy() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return active_ || worker_running_;
}

bool RadioPlayer::IsCancelled(uint32_t playback_id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return cancelled_ || !active_ || playback_id != playback_id_;
}

bool RadioPlayer::SleepUnlessCancelled(uint32_t playback_id, int ms) const {
    for (int waited = 0; waited < ms; waited += 100) {
        if (IsCancelled(playback_id)) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return !IsCancelled(playback_id);
}

void RadioPlayer::WorkerEntry(void* arg) {
    auto* player = static_cast<RadioPlayer*>(arg);
    player->WorkerTask();
    vTaskDelete(nullptr);
}

void RadioPlayer::WorkerTask() {
    std::string url;
    uint32_t playback_id = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        url = url_;
        playback_id = playback_id_;
    }

    RegisterDecodersOnce();
    std::vector<uint8_t> in_buf(kReadBufferSize);
    std::vector<uint8_t> out_buf(4608 * 2);
    int failures = 0;
    bool ever_played = false;

    while (!IsCancelled(playback_id) && failures < kMaxConsecutiveFailures) {
        std::string content_type;
        int fd = OpenStream(url, content_type);
        if (fd < 0) {
            failures++;
            SleepUnlessCancelled(playback_id, 2000);
            continue;
        }
        ESP_LOGI(TAG, "Connected, content-type: %s", content_type.c_str());

        esp_audio_simple_dec_handle_t decoder = nullptr;
        PcmSink sink(audio_service_, playback_id);
        bool got_audio = false;
        int idle_ms = 0;
        esp_audio_simple_dec_info_t info = {};

        while (!IsCancelled(playback_id)) {
            int n = recv(fd, in_buf.data(), in_buf.size(), 0);
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                idle_ms += kReadTimeoutMs;
                if (idle_ms >= kNoDataReconnectMs) {
                    ESP_LOGW(TAG, "No data for %d ms, reconnecting", idle_ms);
                    break;
                }
                continue;
            }
            if (n <= 0) {
                ESP_LOGW(TAG, "Stream closed (%d)", n);
                break;
            }
            idle_ms = 0;

            if (decoder == nullptr) {
                auto type = TypeFromContentType(content_type);
                if (type == ESP_AUDIO_SIMPLE_DEC_TYPE_NONE) {
                    type = SniffType(in_buf.data(), n);
                }
                esp_audio_simple_dec_cfg_t cfg = {
                    .dec_type = type,
                    .dec_cfg = nullptr,
                    .cfg_size = 0,
                    .use_frame_dec = false,
                };
                if (esp_audio_simple_dec_open(&cfg, &decoder) != ESP_AUDIO_ERR_OK) {
                    ESP_LOGE(TAG, "Failed to open %s decoder",
                             esp_audio_simple_dec_get_name(type));
                    decoder = nullptr;
                    break;
                }
                ESP_LOGI(TAG, "Decoder: %s", esp_audio_simple_dec_get_name(type));
            }

            esp_audio_simple_dec_raw_t raw = {
                .buffer = in_buf.data(),
                .len = (uint32_t)n,
                .eos = false,
                .consumed = 0,
                .frame_recover = ESP_AUDIO_SIMPLE_DEC_RECOVERY_NONE,
            };
            bool sink_ok = true;
            while (raw.len > 0 && sink_ok && !IsCancelled(playback_id)) {
                esp_audio_simple_dec_out_t out = {
                    .buffer = out_buf.data(),
                    .len = (uint32_t)out_buf.size(),
                    .needed_size = 0,
                    .decoded_size = 0,
                };
                auto ret = esp_audio_simple_dec_process(decoder, &raw, &out);
                if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
                    out_buf.resize(out.needed_size);
                    continue;
                }
                if (ret != ESP_AUDIO_ERR_OK) {
                    // Skip corrupt data and carry on with the rest of the stream
                    ESP_LOGW(TAG, "Decode error %d, skipping", ret);
                    break;
                }
                if (out.decoded_size > 0) {
                    if (!got_audio || info.sample_rate == 0) {
                        esp_audio_simple_dec_get_info(decoder, &info);
                        ESP_LOGI(TAG, "Stream: %lu Hz, %d ch, %d bit",
                                 (unsigned long)info.sample_rate, info.channel,
                                 info.bits_per_sample);
                    }
                    got_audio = true;
                    sink_ok = sink.Write(out.buffer, out.decoded_size, info.sample_rate,
                                         info.channel, info.bits_per_sample);
                }
                if (raw.consumed == 0 && out.decoded_size == 0) {
                    break;  // decoder needs more input
                }
                raw.buffer += raw.consumed;
                raw.len -= raw.consumed;
            }
            if (!sink_ok) {
                break;  // playback was reset (stopped)
            }
        }

        if (decoder) {
            esp_audio_simple_dec_close(decoder);
        }
        close(fd);

        if (got_audio) {
            ever_played = true;
            failures = 0;
        } else {
            failures++;
        }
        if (!IsCancelled(playback_id)) {
            SleepUnlessCancelled(playback_id, 1000);
        }
    }

    FinishedCallback callback;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        worker_running_ = false;
        task_handle_ = nullptr;
        if (!cancelled_ && active_ && playback_id == playback_id_) {
            active_ = false;
            callback = std::move(finished_callback_);
            finished_callback_ = nullptr;
        }
    }
    ESP_LOGI(TAG, "Radio worker finished (played=%d)", ever_played);
    if (callback) {
        callback(playback_id, false);
    }
}
