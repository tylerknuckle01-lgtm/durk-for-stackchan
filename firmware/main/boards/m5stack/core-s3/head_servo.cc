#include <string>
#include <cctype>
#include "head_servo.h"
#include "application.h"
#include "mcp_server.h"
#include "settings.h"
#include <esp_log.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <cstring>
#include <cmath>
#include <cstdlib>

#define TAG "HeadServo"

extern std::atomic<uint16_t> g_audio_output_level;  // audio_service.cc

namespace {
constexpr uart_port_t kUart = UART_NUM_1;
constexpr int kBaud = 1000000;
constexpr int kTxPin = 6, kRxPin = 7;
constexpr uint8_t kYawId = 1, kPitchId = 2;
constexpr uint8_t kPy32Addr = 0x6F;
// M5 StackChan reference firmware: pitch zero (level) = raw 620, +angle = up, 3.2 raw/deg.
constexpr int kNeutralPitchRaw = 0;
constexpr int kAddrGoal = 42, kAddrPresent = 56, kAddrTorque = 40;

int YawDegToPos(int deg) { int p = 460 + deg * 16 / 5; return p < 0 ? 0 : p > 1000 ? 1000 : p; }
int PitchDegToPos(int api_deg) {
    int raw = kNeutralPitchRaw + api_deg;
    if (raw < 0) raw = 0;
    if (raw > 88) raw = 88;
    return 620 + raw * 16 / 5;
}
int YawPosToDeg(int pos) { return (pos - 460) * 5 / 16; }
int PitchPosToDeg(int pos) { return (pos - 620) * 5 / 16 - kNeutralPitchRaw; }
int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
int64_t now_us() { return esp_timer_get_time(); }
int rnd(int lo, int hi) { return lo + (int)(esp_random() % (uint32_t)(hi - lo + 1)); }

bool Py32Write(i2c_master_dev_handle_t d, uint8_t reg, uint8_t v) {
    uint8_t b[2] = {reg, v};
    for (int i = 0; i < 3; i++) {
        if (i2c_master_transmit(d, b, 2, 100) == ESP_OK) return true;
        vTaskDelay(pdMS_TO_TICKS(15));
    }
    return false;
}
bool Py32Read(i2c_master_dev_handle_t d, uint8_t reg, uint8_t* v) {
    for (int i = 0; i < 3; i++) {
        if (i2c_master_transmit_receive(d, &reg, 1, v, 1, 100) == ESP_OK) return true;
        vTaskDelay(pdMS_TO_TICKS(15));
    }
    return false;
}
bool Py32SetBit(i2c_master_dev_handle_t d, uint8_t reg, int bit, bool on) {
    uint8_t v;
    if (!Py32Read(d, reg, &v)) return false;
    v = on ? (v | (1u << bit)) : (v & ~(1u << bit));
    return Py32Write(d, reg, v);
}
}  // namespace

bool HeadServo::PowerEnable(bool on) {
    if (!py32_) return false;
    // PY32 regs: 0x03 dir L, 0x05 out L, 0x09 pull-up L, 0x0B pull-down L. Pin 0 = servo VM EN.
    bool ok = Py32SetBit(py32_, 0x03, 0, true) && Py32SetBit(py32_, 0x0B, 0, false) &&
              Py32SetBit(py32_, 0x09, 0, true) && Py32SetBit(py32_, 0x05, 0, on);
    uint8_t out = 0;
    Py32Read(py32_, 0x05, &out);
    ESP_LOGI(TAG, "Servo power %s: %s (out_l=0x%02X)", on ? "ON" : "OFF", ok ? "ok" : "FAILED", out);
    return ok;
}

static void SendPacket(uint8_t id, uint8_t inst, const uint8_t* params, int n) {
    uint8_t buf[16];
    buf[0] = 0xFF; buf[1] = 0xFF; buf[2] = id; buf[3] = (uint8_t)(n + 2); buf[4] = inst;
    uint8_t sum = id + buf[3] + inst;
    for (int i = 0; i < n; i++) { buf[5 + i] = params[i]; sum += params[i]; }
    buf[5 + n] = (uint8_t)~sum;
    uart_flush_input(kUart);
    uart_write_bytes(kUart, (const char*)buf, 6 + n);
    uart_wait_tx_done(kUart, pdMS_TO_TICKS(10));
}

// Reads a status packet; returns number of param bytes copied, -1 on error.
static int ReadStatus(uint8_t id, uint8_t* out, int want) {
    uint8_t b = 0, prev = 0;
    for (int i = 0; i < 12; i++) {  // find FF FF header (skips any TX echo bytes)
        if (uart_read_bytes(kUart, &b, 1, pdMS_TO_TICKS(20)) != 1) return -1;
        if (prev == 0xFF && b == 0xFF) break;
        prev = b;
        if (i == 11) return -1;
    }
    uint8_t hdr[3];
    if (uart_read_bytes(kUart, hdr, 3, pdMS_TO_TICKS(20)) != 3) return -1;
    if (hdr[0] == 0xFF) {  // tolerate a third FF
        hdr[0] = hdr[1]; hdr[1] = hdr[2];
        if (uart_read_bytes(kUart, &hdr[2], 1, pdMS_TO_TICKS(20)) != 1) return -1;
    }
    if (hdr[0] != id) return -1;
    int n = hdr[1] - 2;
    if (n < 0 || n > 8) return -1;
    uint8_t data[9];
    if (uart_read_bytes(kUart, data, n + 1, pdMS_TO_TICKS(20)) != n + 1) return -1;
    uint8_t sum = hdr[0] + hdr[1] + hdr[2];
    for (int i = 0; i < n; i++) sum += data[i];
    if ((uint8_t)~sum != data[n]) return -1;
    int c = n < want ? n : want;
    if (out) memcpy(out, data, c);
    return c;
}

bool HeadServo::WritePos(uint8_t id, int pos, int time_ms) {
    for (int attempt = 0; attempt < 3; attempt++) {
        uint8_t p[7] = {kAddrGoal, (uint8_t)(pos >> 8), (uint8_t)pos, (uint8_t)(time_ms >> 8),
                        (uint8_t)time_ms, 0, 0};
        SendPacket(id, 0x03, p, 7);
        if (ReadStatus(id, nullptr, 0) >= 0) { consecutive_fail_ = 0; return true; }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    consecutive_fail_++;
    ESP_LOGW(TAG, "WritePos id=%d pos=%d failed (consecutive=%d)", id, pos, consecutive_fail_);
    return false;
}

int HeadServo::ReadPos(uint8_t id) {
    for (int attempt = 0; attempt < 3; attempt++) {
        uint8_t p[2] = {kAddrPresent, 2};
        SendPacket(id, 0x02, p, 2);
        uint8_t d[2];
        if (ReadStatus(id, d, 2) == 2) return (d[0] << 8) | d[1];
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return -1;
}

void HeadServo::Initialize(i2c_master_bus_handle_t i2c_bus) {
    i2c_device_config_t cfg = {};
    cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    cfg.device_address = kPy32Addr;
    cfg.scl_speed_hz = 100000;
    if (i2c_master_bus_add_device(i2c_bus, &cfg, &py32_) != ESP_OK) py32_ = nullptr;

    uart_config_t uc = {};
    uc.baud_rate = kBaud;
    uc.data_bits = UART_DATA_8_BITS;
    uc.parity = UART_PARITY_DISABLE;
    uc.stop_bits = UART_STOP_BITS_1;
    uc.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uc.source_clk = UART_SCLK_DEFAULT;
    if (uart_driver_install(kUart, 256, 256, 0, nullptr, 0) != ESP_OK ||
        uart_param_config(kUart, &uc) != ESP_OK ||
        uart_set_pin(kUart, kTxPin, kRxPin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) != ESP_OK) {
        ESP_LOGE(TAG, "UART init failed; head disabled");
        return;
    }
    Settings s("head", false);
    auto_motion_ = HEAD_AUTO_MOTION && s.GetBool("auto", true);

    queue_ = xQueueCreate(8, sizeof(Cmd));
    // Low priority on core 1 (below LVGL=4); task mostly sleeps.
    xTaskCreatePinnedToCore([](void* a) { static_cast<HeadServo*>(a)->Task(); }, "head_servo",
                            4096, this, 2, nullptr, 1);
    ok_ = true;
}

bool HeadServo::MoveAxis(uint8_t id, int from, int to, int speed) {
    if (from == to) return true;
    int dist = abs(to - from);
    int time_ms = clampi(dist * 1000 / speed, 150, 4000);
    int pos = (id == kYawId) ? YawDegToPos(to) : PitchDegToPos(to);
    busy_until_us_ = now_us() + (time_ms + 250) * 1000LL;
    bool ok = WritePos(id, pos, time_ms);  // servo times the move itself (single shot)
    if (id == kPitchId) ESP_LOGI(TAG, "pitch %d -> %d deg (raw %d)", from, to, pos);
    if (ok) {
        (id == kYawId ? cur_yaw_ : cur_pitch_) = to;
        vTaskDelay(pdMS_TO_TICKS(time_ms + 40));
    }
    return ok;
}

// Durk: eased (smoothstep) move in a few timed segments -> slow-fast-slow, less robotic.
bool HeadServo::MoveAxisEased(uint8_t id, int from, int to, int speed) {
    int dist = abs(to - from);
    if (dist < 6) return MoveAxis(id, from, to, speed);
    const int n = 5;
    int total = clampi(dist * 1000 / speed, 300, 4000);
    int seg = total / n;
    bool ok = true;
    busy_until_us_ = now_us() + (total + 300) * 1000LL;
    for (int i = 1; i <= n && ok; i++) {
        float t = (float)i / n, s = t * t * (3 - 2 * t);
        int d = from + (int)lroundf((to - from) * s);
        int pos = (id == kYawId) ? YawDegToPos(d) : PitchDegToPos(d);
        ok = WritePos(id, pos, seg + 20);
        if (ok) { (id == kYawId ? cur_yaw_ : cur_pitch_) = d; vTaskDelay(pdMS_TO_TICKS(seg)); }
    }
    busy_until_us_ = now_us() + 250000;
    return ok;
}

void HeadServo::Execute(const Cmd& c) {
    if (fault_) return;
    int y = clampi(c.yaw, kYawMin, kYawMax), p = clampi(c.pitch, kPitchMin, kPitchMax);
    int sp = clampi(c.speed, 10, 90);
    // One axis at a time to limit current spikes.
    if (c.ease) {
        MoveAxisEased(kYawId, cur_yaw_, y, sp);
        vTaskDelay(pdMS_TO_TICKS(20));
        MoveAxisEased(kPitchId, cur_pitch_, p, sp);
    } else {
    MoveAxis(kYawId, cur_yaw_, y, sp);
    vTaskDelay(pdMS_TO_TICKS(20));
    MoveAxis(kPitchId, cur_pitch_, p, sp);
    }
    if (consecutive_fail_ >= 3) {
        if (!power_cycled_) {
            // Bus-hang recovery: power-cycle servo rail once via PY32, then re-sync.
            ESP_LOGW(TAG, "Servo bus not responding; power-cycling servo rail");
            power_cycled_ = true;
            PowerEnable(false);
            vTaskDelay(pdMS_TO_TICKS(500));
            PowerEnable(true);
            vTaskDelay(pdMS_TO_TICKS(1500));
            int yp = ReadPos(kYawId), pp = ReadPos(kPitchId);
            if (yp >= 0) cur_yaw_ = YawPosToDeg(yp);
            if (pp >= 0) cur_pitch_ = PitchPosToDeg(pp);
            consecutive_fail_ = (yp >= 0 && pp >= 0) ? 0 : 3;
        }
        if (consecutive_fail_ >= 3) {
            ESP_LOGE(TAG, "Servo fault: motion disabled until reboot");
            fault_ = true;
        }
    }
}

// Durk: equivalent of the stock M5 StackChan servo routine (setup "servo test": home, left, home,
// right, home, up, home) plus read-back verification against the stock zero positions
// (NVS servo:zero_pos_1=460, zero_pos_2=620 in the stock backup).
void HeadServo::Home(bool sweep) {
    if (fault_) return;
    ESP_LOGI(TAG, "Homing: start (sweep=%d)", sweep);
    manual_until_us_ = now_us() + 20000000;
    face_until_us_ = 0;
    for (uint8_t id : {kYawId, kPitchId}) {  // torque on
        uint8_t p[2] = {(uint8_t)kAddrTorque, 1};
        SendPacket(id, 0x03, p, 2);
        ReadStatus(id, nullptr, 0);
    }
    if (sweep) {
        Execute({-40, 0, 60, 1});
        vTaskDelay(pdMS_TO_TICKS(300));
        Execute({0, 0, 60, 1});
        Execute({40, 0, 60, 1});
        vTaskDelay(pdMS_TO_TICKS(300));
        Execute({0, 0, 60, 1});
        Execute({0, 25, 40, 1});
        vTaskDelay(pdMS_TO_TICKS(300));
    }
    Execute({0, 0, 30, 1});
    const int ty = YawDegToPos(0), tp = PitchDegToPos(0);
    int yp = -1, pp = -1;
    for (int attempt = 0; attempt < 3; attempt++) {
        vTaskDelay(pdMS_TO_TICKS(500));  // settle
        yp = ReadPos(kYawId);
        pp = ReadPos(kPitchId);
        bool ok = true;
        if (yp >= 0 && abs(yp - ty) > 3) { WritePos(kYawId, ty, 300); ok = false; }
        if (pp >= 0 && abs(pp - tp) > 3) { WritePos(kPitchId, tp, 300); ok = false; }
        if (ok) break;
    }
    cur_yaw_ = 0;
    cur_pitch_ = 0;
    last_home_yaw_ = yp;
    last_home_pitch_ = pp;
    ESP_LOGI(TAG, "Homing: done. yaw raw=%d (target %d), pitch raw=%d (target %d)", yp, ty, pp, tp);
    manual_until_us_ = now_us() + 3000000;
    homed_ = true;
}

void HeadServo::Task() {
    PowerEnable(true);
    vTaskDelay(pdMS_TO_TICKS(1500));  // SCS0009 wake-up latency
    int yp = ReadPos(kYawId), pp = ReadPos(kPitchId);
    ESP_LOGI(TAG, "Boot positions: yaw pos=%d pitch pos=%d", yp, pp);
    cur_yaw_ = yp >= 0 ? YawPosToDeg(yp) : 0;
    cur_pitch_ = pp >= 0 ? PitchPosToDeg(pp) : 0;
    if (yp < 0 && pp < 0) ESP_LOGW(TAG, "No servo replies at boot; will still try to center slowly");
    // Center slowly (15 deg/s), one axis at a time.
    Execute({0, 0, 15});
    next_glance_us_ = now_us() + 8000000;

    Cmd c;
    while (true) {
        if (home_req_.exchange(false) || (!homed_ && now_us() > 9000000)) {
            xQueueReset(queue_);
            Home(true);
            continue;
        }
        if (xQueueReceive(queue_, &c, pdMS_TO_TICKS(100)) == pdTRUE) {
            Execute(c);
            continue;
        }
        if (HEAD_AUTO_MOTION && auto_motion_ && !fault_) AutoTick();
    }
}

void HeadServo::AutoTick() {
    auto& app = Application::GetInstance();
    auto st = app.GetDeviceState();
    int64_t t = now_us();
    if (st == kDeviceStateSpeaking) {
        // Small speech-level nod (pitch only), with cooldown.
        if (t > next_nod_us_ && g_audio_output_level.load(std::memory_order_relaxed) > 2500) {
            int base = cur_pitch_;
            MoveAxis(kPitchId, base, clampi(base + 5, kPitchMin, kPitchMax), 50);
            MoveAxis(kPitchId, cur_pitch_, base, 50);
            next_nod_us_ = now_us() + rnd(1800, 3000) * 1000LL;
        }
        return;
    }
    if (ManualHold()) return;  // honour a manual look for a while
    if (FaceActive()) return;  // face tracker is steering
    if (st == kDeviceStateListening) {
        // Face the user while listening.
        if (cur_yaw_ != 0 || cur_pitch_ != kLevelPitch) Execute({0, kLevelPitch, 40});
        return;
    }
    if (st != kDeviceStateIdle || sleeping_) return;  // paused in sleep
    if (t < next_glance_us_) return;
    // Gentle random glance, then often drift back toward center.
    int y = (esp_random() % 3 == 0) ? 0 : rnd(-25, 25);
    int p = rnd(0, 12);
    Execute({(int16_t)y, (int16_t)p, (int16_t)rnd(20, 35)});
    next_glance_us_ = now_us() + rnd(4000, 10000) * 1000LL;
}

void HeadServo::NoteFace() { face_until_us_ = now_us() + 2500000; }
bool HeadServo::Busy() const { return now_us() < busy_until_us_.load() || uxQueueMessagesWaiting(queue_) > 0; }
bool HeadServo::ManualHold() const { return now_us() < manual_until_us_.load(); }
bool HeadServo::FaceActive() const { return now_us() < face_until_us_.load(); }
bool HeadServo::Track(int yaw, int pitch, int speed) {
    if (ManualHold()) return false;
    NoteFace();
    if (!ok_ || fault_) return false;
    if (uxQueueMessagesWaiting(queue_) > 0) return false;  // previous correction still pending
    Cmd c{(int16_t)clampi(yaw, kYawMin, kYawMax), (int16_t)clampi(pitch, kPitchMin, kPitchMax), (int16_t)speed};
    return xQueueSend(queue_, &c, 0) == pdTRUE;
}

bool HeadServo::MoveTo(int yaw, int pitch, int speed) {
    if (!ok_ || fault_) return false;
    Cmd c{(int16_t)yaw, (int16_t)pitch, (int16_t)speed};
    manual_until_us_ = now_us() + 10000000;  // pause face tracking 10 s
    face_until_us_ = 0;
    ESP_LOGI(TAG, "manual move yaw=%d pitch=%d", yaw, pitch);
    next_glance_us_ = now_us() + 15000000;  // commanded pose holds before idle glances resume
    return xQueueSend(queue_, &c, 0) == pdTRUE;
}

bool HeadServo::Look(const char* din) {
    // Directions are from the USER's point of view, facing the robot.
    // Positive yaw = robot's own left = user's right.
    std::string d(din ? din : "");
    for (auto& ch : d) ch = (char)tolower((unsigned char)ch);
    while (!d.empty() && isspace((unsigned char)d.back())) d.pop_back();
    while (!d.empty() && isspace((unsigned char)d.front())) d.erase(0, 1);
    ESP_LOGI(TAG, "look '%s'", d.c_str());
    if (d == "left") return MoveTo(-45, cur_pitch_, 40);
    if (d == "right") return MoveTo(45, cur_pitch_, 40);
    if (d == "up") return MoveTo(cur_yaw_, 25, 30);
    if (d == "down") return MoveTo(cur_yaw_, 0, 30);
    if (d == "center" || d == "forward" || d == "centre" || d == "straight") return MoveTo(0, kLevelPitch, 40);
    return false;
}

void HeadServo::Gesture(int dy, int dp, int kind) {
    if (!ok_ || fault_ || sleeping_ || !auto_motion_) return;
    if (ManualHold() || uxQueueMessagesWaiting(queue_) > 0) return;  // never fight a commanded look
    int y = cur_yaw_, p = cur_pitch_;
    int ty = clampi(y + dy, kYawMin, kYawMax), tp = clampi(p + dp, kPitchMin, kPitchMax);
    if (kind == 1 && tp == p) tp = clampi(p - dp, kPitchMin, kPitchMax);  // at a limit: go the other way
    manual_until_us_ = now_us() + 2500000;  // brief tracker pause only
    Cmd a{(int16_t)ty, (int16_t)tp, 50, 1}, back{(int16_t)y, (int16_t)p, 40, 1}, b2{(int16_t)clampi(y - dy, kYawMin, kYawMax), (int16_t)p, 60, 1};
    xQueueSend(queue_, &a, 0);
    if (kind == 1) xQueueSend(queue_, &back, 0);
    if (kind == 2) { xQueueSend(queue_, &b2, 0); xQueueSend(queue_, &back, 0); }
}

bool HeadServo::QueueSeq(const int (*pts)[2], int n, int speed) {
    if (!ok_ || fault_) return false;
    int y = cur_yaw_, p = cur_pitch_;
    manual_until_us_ = now_us() + 4000000;  // short tracker pause
    for (int i = 0; i < n; i++) {
        Cmd c{(int16_t)clampi(y + pts[i][0], kYawMin, kYawMax), (int16_t)clampi(p + pts[i][1], kPitchMin, kPitchMax), (int16_t)speed, 1};
        if (xQueueSend(queue_, &c, 0) != pdTRUE) return false;
    }
    Cmd back{(int16_t)y, (int16_t)p, (int16_t)speed, 1};
    return xQueueSend(queue_, &back, 0) == pdTRUE;
}

bool HeadServo::GestureNamed(const char* g) {
    if (!g) return false;
    // offsets relative to current pose {dyaw, dpitch}; pitch only goes up from level, so "down" gestures dip from where it is.
    static const int nod[][2] = {{0, 10}, {0, 0}, {0, 7}};
    static const int shake[][2] = {{-18, 0}, {18, 0}, {-10, 0}};
    static const int tilt[][2] = {{14, 8}};
    static const int happy[][2] = {{0, 8}, {6, 3}, {-6, 8}};
    static const int sad[][2] = {{0, -8}, {-8, -8}};
    static const int think[][2] = {{16, 10}, {16, 12}};
    static const int surprise[][2] = {{0, 14}};
    static const int dizzy[][2] = {{-15, 6}, {0, 12}, {15, 6}, {0, 0}, {-8, 4}};
    static const int bow[][2] = {{0, -12}, {0, -12}};
    static const int sleepy[][2] = {{0, -10}, {6, -12}};
    static const int love[][2] = {{-10, 6}, {10, 6}};
#define G(name, arr, sp) if (!strcmp(g, name)) return QueueSeq(arr, sizeof(arr) / sizeof(arr[0]), sp);
    G("nod", nod, 70) G("shake", shake, 80) G("tilt", tilt, 40) G("happy", happy, 60) G("sad", sad, 25)
    G("think", think, 30) G("surprise", surprise, 90) G("dizzy", dizzy, 60) G("bow", bow, 30)
    G("sleepy", sleepy, 15) G("love", love, 35)
#undef G
    return false;
}

bool HeadServo::Nod() {
    int y = cur_yaw_;
    return MoveTo(y, 12, 60) && MoveTo(y, 0, 60) && MoveTo(y, 8, 60) && MoveTo(y, kLevelPitch, 60);
}

bool HeadServo::Shake() {
    int p = cur_pitch_;
    return MoveTo(25, p, 70) && MoveTo(-25, p, 70) && MoveTo(15, p, 70) && MoveTo(0, p, 60);
}

void HeadServo::SetAutoMotion(bool on) {
    auto_motion_ = on;
    Settings s("head", true);
    s.SetBool("auto", on);
}

void HeadServo::RegisterMcpTools() {
    auto& mcp = McpServer::GetInstance();
    mcp.AddTool("robot.recalibrate_head",
                "Recalibrate / re-home the head: sweeps left, right, up, then centers level and verifies the servo positions. "
                "Use when the user says recalibrate head, home your head, or your head is crooked.",
                PropertyList(), [this](const PropertyList&) -> ReturnValue {
                    RequestHome();
                    return std::string("Recalibrating head now (takes ~10 s). Last home read-back: yaw raw ") +
                           std::to_string(last_home_yaw_.load()) + " (target 460), pitch raw " + std::to_string(last_home_pitch_.load()) + " (target 620).";
                });
    mcp.AddTool("robot.gesture",
                "Do a smooth expressive head gesture: nod, shake, tilt, happy, sad, think, surprise, dizzy, bow, sleepy, love.",
                PropertyList({Property("name", kPropertyTypeString)}),
                [this](const PropertyList& props) -> ReturnValue {
                    auto n = props["name"].value<std::string>();
                    return GestureNamed(n.c_str());
                });
    mcp.AddTool("robot.look",
                "Turn the robot's head. direction: left, right, up, down, or center. left/right are from the USER's point of view (the person facing the robot): 'left' turns toward the user's left. Use exactly what the user said; do not mirror it.",
                PropertyList({Property("direction", kPropertyTypeString)}),
                [this](const PropertyList& props) -> ReturnValue {
                    auto d = props["direction"].value<std::string>();
                    return Look(d.c_str());
                });
    mcp.AddTool("robot.set_head_angles",
                "Set head angles. yaw -60..60: NEGATIVE = turn toward the USER's left, POSITIVE = toward the USER's right "
                "(user facing the robot); prefer robot.look for plain left/right. "
                "pitch 0..30 (0 = level, positive = look up), speed 10..90 deg/s.",
                PropertyList({Property("yaw", kPropertyTypeInteger, 0, kYawMin, kYawMax),
                              Property("pitch", kPropertyTypeInteger, kLevelPitch, kPitchMin, kPitchMax),
                              Property("speed", kPropertyTypeInteger, 40, 10, 90)}),
                [this](const PropertyList& props) -> ReturnValue {
                    return MoveTo(props["yaw"].value<int>(), props["pitch"].value<int>(),
                                  props["speed"].value<int>());
                });
    mcp.AddTool("robot.get_head_angles",
                "Returns current head yaw/pitch in degrees. Neutral is {yaw:0, pitch:0}.",
                PropertyList(), [this](const PropertyList&) -> ReturnValue {
                    char buf[96];
                    snprintf(buf, sizeof(buf), "{\"yaw\":%d,\"pitch\":%d,\"ok\":%s}",
                             (int)cur_yaw_, (int)cur_pitch_, Ok() ? "true" : "false");
                    return std::string(buf);
                });
    mcp.AddTool("robot.nod", "Nod the head (yes).", PropertyList(),
                [this](const PropertyList&) -> ReturnValue { return Nod(); });
    mcp.AddTool("robot.shake_head", "Shake the head (no).", PropertyList(),
                [this](const PropertyList&) -> ReturnValue { return Shake(); });
    mcp.AddTool("robot.look_around",
                "Enable or disable automatic head motion (idle look-around and nods while "
                "speaking). Setting persists.",
                PropertyList({Property("enabled", kPropertyTypeBoolean)}),
                [this](const PropertyList& props) -> ReturnValue {
                    SetAutoMotion(props["enabled"].value<bool>());
                    return true;
                });
}
