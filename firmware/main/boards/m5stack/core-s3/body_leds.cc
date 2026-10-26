#include "body_leds.h"
#include "application.h"
#include "assets/lang_config.h"
#include <memory>
#include "mcp_server.h"
#include "settings.h"
#include "board.h"
#include "display.h"
#include "head_servo.h"
#include "durk_sd.h"
#include "system_info.h"
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <cmath>
#include <cstring>

#define TAG "BodyLeds"
extern std::atomic<uint16_t> g_audio_output_level;  // audio_service.cc

namespace {
constexpr int kDataPin = 13;           // PY32 pin (bit 5 of high-byte regs)
constexpr uint8_t kRegLedCfg = 0x24, kRegLedRam = 0x30;
constexpr int kMaxChannel = 168;       // same safe cap as the original firmware
constexpr int kTickMs = 50;            // 20 Hz

bool Wr(i2c_master_dev_handle_t d, const uint8_t* b, size_t n) {
    for (int i = 0; i < 3; i++) {
        if (i2c_master_transmit(d, b, n, 50) == ESP_OK) return true;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return false;
}
bool SetBit(i2c_master_dev_handle_t d, uint8_t reg, int bit, bool on) {
    uint8_t v = 0;
    if (i2c_master_transmit_receive(d, &reg, 1, &v, 1, 100) != ESP_OK) return false;
    v = on ? (v | (1u << bit)) : (v & ~(1u << bit));
    uint8_t b[2] = {reg, v};
    return Wr(d, b, 2);
}
enum Mood { kMoodNone = 0, kMoodHappy, kMoodSad, kMoodSurprised, kMoodAngry, kMoodThinking, kMoodLaughing };
void Hue(float h, float k, uint8_t* p) {  // h 0..1
    float r = fabsf(h * 6 - 3) - 1, g = 2 - fabsf(h * 6 - 2), b = 2 - fabsf(h * 6 - 4);
    auto c = [](float v) { return v < 0 ? 0.f : v > 1 ? 1.f : v; };
    p[0] = (uint8_t)(255 * c(r) * k); p[1] = (uint8_t)(255 * c(g) * k); p[2] = (uint8_t)(255 * c(b) * k);
}
}  // namespace

void BodyLeds::Initialize(i2c_master_dev_handle_t py32) {
    py32_ = py32;
    if (!py32_) { ESP_LOGW(TAG, "No PY32 handle; LEDs disabled"); return; }
    Settings s("leds", false);
    brightness_ = s.GetInt("bright", 15);
    xTaskCreatePinnedToCore([](void* a) { static_cast<BodyLeds*>(a)->Task(); }, "body_leds", 3072,
                            this, 1, nullptr, 1);
}

bool BodyLeds::Write(const uint8_t rgb[kCount][3]) {
    uint8_t buf[1 + kCount * 2];
    buf[0] = kRegLedRam;
    int br = brightness_;
    for (int i = 0; i < kCount; i++) {
        int r = rgb[i][0] * br / 100, g = rgb[i][1] * br / 100, b = rgb[i][2] * br / 100;
        if (r > kMaxChannel) r = kMaxChannel;
        if (g > kMaxChannel) g = kMaxChannel;
        if (b > kMaxChannel) b = kMaxChannel;
        uint16_t v = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
        buf[1 + i * 2] = v & 0xFF;
        buf[2 + i * 2] = v >> 8;
    }
    if (!Wr(py32_, buf, sizeof(buf))) return false;
    uint8_t latch[2] = {kRegLedCfg, (uint8_t)(kCount | (1u << 6))};
    return Wr(py32_, latch, 2);
}

void BodyLeds::Task() {
    vTaskDelay(pdMS_TO_TICKS(2500));  // let the servo task finish PY32 power setup
    int hb = kDataPin - 8;
    bool ok = SetBit(py32_, 0x04, hb, true)     // direction: output
              && SetBit(py32_, 0x0C, hb, false)  // no pull-down
              && SetBit(py32_, 0x0A, hb, true)   // pull-up
              && SetBit(py32_, 0x14, hb, false); // push-pull
    uint8_t cnt[2] = {kRegLedCfg, (uint8_t)kCount};
    ok = ok && Wr(py32_, cnt, 2);
    vTaskDelay(pdMS_TO_TICKS(200));
    uint8_t zero[kCount][3] = {};
    ok = ok && Write(zero);
    if (!ok) { ESP_LOGE(TAG, "LED init failed; LEDs disabled"); vTaskDelete(nullptr); return; }
    ok_ = true;
    ESP_LOGI(TAG, "Body LEDs ready (%d WS2812C via PY32 pin %d)", kCount, kDataPin);

    auto& app = Application::GetInstance();
    DeviceState last_state = kDeviceStateUnknown;
    int64_t flash_until = 0;
    float level_s = 0;
    uint8_t frame[kCount][3], prev[kCount][3];
    memset(prev, 0xFF, sizeof(prev));
    int fails = 0;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(kTickMs));
        int64_t now = esp_timer_get_time();
        float t = now / 1e6f;
        auto st = app.GetDeviceState();
        if (st != last_state) {
            // Quick flash when a wake-word / button starts a conversation.
            if (st == kDeviceStateConnecting && last_state == kDeviceStateIdle) {
                flash_until = now + 250000;
                durk_sd::PlaySfx("wake");
            }
            last_state = st;
        }
        memset(frame, 0, sizeof(frame));
        int64_t tend = timer_end_us_.load();
        if (tend && now >= tend) { timer_end_us_ = 0; FireAlarm(); tend = 0; }
        int mood = now < mood_until_us_.load() ? mood_.load() : kMoodNone;
        float mt = (now - mood_start_us_.load()) / 1e6f;
        if (now < alarm_until_us_.load()) {
            bool red = ((now / 250000) % 2) == 0;
            for (auto& p : frame) { p[0] = 255; p[1] = red ? 0 : 255; p[2] = red ? 0 : 255; }
        } else if (manual_) {
            memcpy(frame, manual_rgb_, sizeof(frame));
        } else if (now < flash_until) {
            for (auto& p : frame) { p[0] = 160; p[1] = 160; p[2] = 200; }
        } else if (sleeping_) {
            // off in sleep
        } else if (st == kDeviceStateListening || st == kDeviceStateConnecting) {
            float k = 0.35f + 0.65f * (0.5f + 0.5f * sinf(t * 2.0f * 3.14159f / 2.5f));
            for (auto& p : frame) { p[0] = 0; p[1] = (uint8_t)(60 * k); p[2] = (uint8_t)(255 * k); }
        } else if (st == kDeviceStateSpeaking) {
            float lvl = g_audio_output_level.load(std::memory_order_relaxed) / 5000.0f;
            if (lvl > 1) lvl = 1;
            level_s += (lvl - level_s) * (lvl > level_s ? 0.5f : 0.2f);
            float k = 0.25f + 0.75f * level_s;
            uint8_t base[3] = {40, 200, 255};  // default cyan speaking pulse; mood tints it
            switch (mood) {
                case kMoodHappy: base[0] = 255; base[1] = 140; base[2] = 30; break;
                case kMoodSad: base[0] = 20; base[1] = 40; base[2] = 160; k *= 0.6f; break;
                case kMoodAngry: base[0] = 255; base[1] = 20; base[2] = 0; break;
                case kMoodThinking: base[0] = 150; base[1] = 40; base[2] = 255;
                    k = 0.3f + 0.4f * (0.5f + 0.5f * sinf(mt * 2.0f * 3.14159f / 3.0f)); break;
                case kMoodSurprised: if (mt < 0.4f) { base[0] = base[1] = base[2] = 255; k = 1; } break;
                default: break;
            }
            if (mood == kMoodLaughing) {
                for (int i = 0; i < kCount; i++) Hue(fmodf(i / (float)kCount + mt * 0.6f, 1.f), k, frame[i]);
            } else {
                for (auto& p : frame) { p[0] = (uint8_t)(base[0] * k); p[1] = (uint8_t)(base[1] * k); p[2] = (uint8_t)(base[2] * k); }
            }
        } else if (st == kDeviceStateIdle) {
            for (auto& p : frame) { p[0] = 0; p[1] = 8; p[2] = 20; }  // very dim
        }
        // Timer countdown ring (amber) overrides idle/sleep/listening, not speech or alarm/manual.
        if (tend && !manual_ && now >= alarm_until_us_.load() && st != kDeviceStateSpeaking) {
            int64_t total = timer_total_us_.load();
            float frac = total > 0 ? (float)(tend - now) / total : 0;
            float lit = frac * kCount;
            memset(frame, 0, sizeof(frame));
            for (int i = 0; i < kCount; i++) {
                float k = lit - i; k = k < 0 ? 0 : k > 1 ? 1 : k;
                frame[i][0] = (uint8_t)(255 * k); frame[i][1] = (uint8_t)(110 * k); frame[i][2] = 0;
            }
        }
        // Only touch the I2C bus when the visible frame changes.
        bool changed = dirty_.exchange(false) || memcmp(frame, prev, sizeof(frame)) != 0;
        if (!changed) continue;
        if (Write(frame)) {
            memcpy(prev, frame, sizeof(frame));
            fails = 0;
        } else if (++fails > 20) {
            ESP_LOGE(TAG, "Repeated LED write failures; LEDs disabled");
            vTaskDelete(nullptr);
            return;
        }
    }
}

void BodyLeds::SetManual(int r, int g, int b, int index) {
    if (index < 0 || index >= kCount) {
        for (auto& p : manual_rgb_) { p[0] = r; p[1] = g; p[2] = b; }
    } else {
        if (!manual_) memset(manual_rgb_, 0, sizeof(manual_rgb_));
        manual_rgb_[index][0] = r; manual_rgb_[index][1] = g; manual_rgb_[index][2] = b;
    }
    manual_ = true;
    dirty_ = true;
}

bool BodyLeds::StartTimer(int seconds, const std::string& label) {
    if (seconds < 1 || seconds > 24 * 3600) return false;
    { std::lock_guard<std::mutex> g(timer_mu_); timer_label_ = label; }
    timer_total_us_ = (int64_t)seconds * 1000000;
    timer_end_us_ = esp_timer_get_time() + (int64_t)seconds * 1000000;
    alarm_until_us_ = 0;
    ESP_LOGI(TAG, "timer set: %d s '%s'", seconds, label.c_str());
    return true;
}

bool BodyLeds::CancelTimer() {
    bool had = timer_end_us_.exchange(0) != 0 || alarm_until_us_.load() > esp_timer_get_time();
    alarm_until_us_ = 0;
    ESP_LOGI(TAG, "timer cancelled (had=%d)", (int)had);
    return had;
}

std::string BodyLeds::TimerStatus() {
    int64_t e = timer_end_us_.load();
    std::string lbl; { std::lock_guard<std::mutex> g(timer_mu_); lbl = timer_label_; }
    if (!e) return "{\"active\":false}";
    int rem = (int)((e - esp_timer_get_time()) / 1000000); if (rem < 0) rem = 0;
    char buf[160];
    snprintf(buf, sizeof(buf), "{\"active\":true,\"remaining_seconds\":%d,\"label\":\"%s\"}", rem, lbl.c_str());
    return buf;
}

void BodyLeds::FireAlarm() {
    alarm_until_us_ = esp_timer_get_time() + 6000000;
    std::string lbl; { std::lock_guard<std::mutex> g(timer_mu_); lbl = timer_label_; }
    ESP_LOGI(TAG, "TIMER DONE '%s'", lbl.c_str());
    durk_sd::PlaySfx("timer");
    Application::GetInstance().Schedule([]() {
        auto d = Board::GetInstance().GetDisplay();
        if (d) d->SetEmotion("surprised");
    });
    // Spoken alert via the Durk server (queued if no live session; poller picks it up even when asleep).
    auto* arg = new std::string(lbl);
    xTaskCreate([](void* a) {
        std::unique_ptr<std::string> l(static_cast<std::string*>(a));
        std::string clean;
        for (char c : *l) if (c != '"' && c != '\\' && (unsigned char)c >= 32) clean += c;
        std::string url;
        { Settings s("wifi", false); url = s.GetString("ota_url"); }
        if (url.empty()) url = CONFIG_OTA_URL;
        auto pos = url.find("/xiaozhi/ota");
        bool ok = false;
        if (pos != std::string::npos) {
            url = url.substr(0, pos) + "/durk/say";
            std::string body = "{\"text\":\"Hey, your " + (clean.empty() ? std::string("") : clean + " ") + "timer is done!\"}";
            auto net = Board::GetInstance().GetNetwork();
            if (net) {
                auto http = net->CreateHttp(0);
                http->SetHeader("Content-Type", "application/json");
                http->SetContent(std::move(body));
                if (http->Open("POST", url)) { auto c = http->GetStatusCode(); ok = c && *c == 200; http->ReadAll(); }
                http->Close();
            }
        }
        ESP_LOGI(TAG, "timer voice alert %s", ok ? "queued" : "FAILED (beep fallback)");
        if (!ok && !durk_sd::HasSfx("timer")) Application::GetInstance().PlaySound(Lang::Sounds::OGG_POPUP);
        vTaskDelete(nullptr);
    }, "timer_say", 6144, arg, 2, nullptr);
}

void BodyLeds::SetMood(const char* e) {
    if (!e) return;
    int m = kMoodNone;
    if (!strcmp(e, "happy") || !strcmp(e, "loving") || !strcmp(e, "cool") || !strcmp(e, "confident")) m = kMoodHappy;
    else if (!strcmp(e, "sad") || !strcmp(e, "crying")) m = kMoodSad;
    else if (!strcmp(e, "surprised") || !strcmp(e, "shocked")) m = kMoodSurprised;
    else if (!strcmp(e, "angry")) m = kMoodAngry;
    else if (!strcmp(e, "thinking") || !strcmp(e, "confused")) m = kMoodThinking;
    else if (!strcmp(e, "laughing") || !strcmp(e, "funny") || !strcmp(e, "silly")) m = kMoodLaughing;
    if (m == kMoodNone) return;
    int64_t now = esp_timer_get_time();
    bool fresh = m != mood_.load() || now > mood_until_us_.load();
    mood_ = m;
    mood_until_us_ = now + 8000000;
    if (!fresh) return;
    mood_start_us_ = now;
    ESP_LOGI(TAG, "mood %s", e);
    auto& head = HeadServo::GetInstance();
    switch (m) {
        case kMoodHappy: head.Gesture(0, 6, 1); break;      // small nod
        case kMoodSad: head.Gesture(0, -6, 1); break;       // head down a bit
        case kMoodThinking: head.Gesture(10, 6, 0); break;  // tilt-ish (yaw + up), hold
        case kMoodLaughing: {
            head.Gesture(8, 0, 2);  // wiggle
            static int64_t last_laugh = -60000000;
            if (now - last_laugh > 30000000) { last_laugh = now; durk_sd::PlaySfx("laugh"); }
            break;
        }
        case kMoodSurprised: head.Gesture(0, 8, 1); break;
        default: break;
    }
}

void BodyLeds::Off() { manual_ = false; dirty_ = true; }

void BodyLeds::SetBrightness(int pct) {
    brightness_ = pct < 0 ? 0 : pct > 100 ? 100 : pct;
    Settings s("leds", true);
    s.SetInt("bright", brightness_);
    dirty_ = true;
}

void BodyLeds::RegisterMcpTools() {
    auto& mcp = McpServer::GetInstance();
    mcp.AddTool("robot.set_led_color",
                "Set the robot's own body LEDs (12 LEDs, NOT room lights). r,g,b 0-255. "
                "index 0-11 for one LED, or -1 for all. Holds until robot.led_off.",
                PropertyList({Property("r", kPropertyTypeInteger, 0, 255),
                              Property("g", kPropertyTypeInteger, 0, 255),
                              Property("b", kPropertyTypeInteger, 0, 255),
                              Property("index", kPropertyTypeInteger, -1, -1, 11)}),
                [this](const PropertyList& p) -> ReturnValue {
                    if (!ok_) return false;
                    SetManual(p["r"].value<int>(), p["g"].value<int>(), p["b"].value<int>(),
                              p["index"].value<int>());
                    return true;
                });
    mcp.AddTool("robot.set_timer",
                "Start a countdown timer on the robot (e.g. 'set a 5 minute timer for the pizza'). seconds 1-86400, "
                "label = short name like 'pizza' (may be empty). The LED ring counts down in amber; at zero the robot "
                "flashes, looks surprised and says the timer is done, even if asleep. One timer at a time; a new one replaces it.",
                PropertyList({Property("seconds", kPropertyTypeInteger, 1, 86400),
                              Property("label", kPropertyTypeString, std::string(""))}),
                [this](const PropertyList& p) -> ReturnValue {
                    return StartTimer(p["seconds"].value<int>(), p["label"].value<std::string>());
                });
    mcp.AddTool("robot.cancel_timer", "Cancel the running timer (or silence a ringing one).", PropertyList(),
                [this](const PropertyList&) -> ReturnValue { return CancelTimer(); });
    mcp.AddTool("robot.get_timer", "How much time is left on the timer. Returns remaining_seconds and label.",
                PropertyList(), [this](const PropertyList&) -> ReturnValue { return TimerStatus(); });
    mcp.AddTool("robot.led_off",
                "Turn off custom LED colors and return the body LEDs to automatic status lighting.",
                PropertyList(), [this](const PropertyList&) -> ReturnValue {
                    Off();
                    return true;
                });
    mcp.AddTool("robot.set_led_brightness",
                "Set body LED brightness 0-100 (default 15; keep low to save power).",
                PropertyList({Property("brightness", kPropertyTypeInteger, 0, 100)}),
                [this](const PropertyList& p) -> ReturnValue {
                    SetBrightness(p["brightness"].value<int>());
                    return true;
                });
}
