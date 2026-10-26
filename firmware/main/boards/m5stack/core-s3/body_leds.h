// Stack-chan body RGB LEDs: 12x WS2812C driven by the PY32 IO expander (pin 13),
// written over I2C as RGB565 into PY32 LED RAM (0x30..), latched via REG 0x24 bit 6.
// Register map from carbeso/stackchan-mcp (ported from M5Stack BSP PY32IOExpander).
#pragma once
#include <driver/i2c_master.h>
#include <atomic>
#include <cstdint>
#include <string>
#include <mutex>

class BodyLeds {
public:
    static constexpr int kCount = 12;
    static BodyLeds& GetInstance() { static BodyLeds s; return s; }
    void Initialize(i2c_master_dev_handle_t py32);
    void RegisterMcpTools();
    void SetManual(int r, int g, int b, int index);  // index -1 = all
    void Off();                                       // back to automatic states
    void SetBrightness(int pct);
    void SetSleeping(bool s) { sleeping_ = s; }
    // Durk timers (LED countdown ring + alarm) and moods
    bool StartTimer(int seconds, const std::string& label);
    bool CancelTimer();
    std::string TimerStatus();
    void SetMood(const char* emotion);

private:
    BodyLeds() = default;
    void Task();
    bool Write(const uint8_t rgb[kCount][3]);
    i2c_master_dev_handle_t py32_ = nullptr;
    bool ok_ = false;
    std::atomic<int> brightness_{15};
    std::atomic<bool> sleeping_{false};
    std::atomic<bool> manual_{false};
    uint8_t manual_rgb_[kCount][3] = {};
    std::atomic<bool> dirty_{true};
    std::atomic<int64_t> timer_end_us_{0}, timer_total_us_{0}, alarm_until_us_{0};
    std::mutex timer_mu_;
    std::string timer_label_;
    std::atomic<int> mood_{0};
    std::atomic<int64_t> mood_until_us_{0}, mood_start_us_{0};
    void FireAlarm();
};
