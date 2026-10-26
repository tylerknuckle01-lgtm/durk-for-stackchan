// Stack-chan head (2x Feetech SCS0009 on a serial bus) for M5Stack CoreS3 Stack-chan kit.
// Clean-room minimal SCS protocol implementation (MIT, part of this firmware).
// Hardware facts (pins, baud, IDs, PY32 VM-EN pin, pos mapping) taken from
// carbeso/stackchan-mcp firmware/main/boards/stackchan/{config.h,stackchan.cc}.
#pragma once
#include <driver/i2c_master.h>
#include <driver/uart.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <atomic>

// Set to 0 to compile out all automatic motion (idle look-around + speech nods).
#ifndef HEAD_AUTO_MOTION
#define HEAD_AUTO_MOTION 1
#endif

class HeadServo {
public:
    // API angles: yaw -60..60 (+ = robot's own left = USER's right), pitch 0..30 (0 = level, + = up)
    static constexpr int kYawMin = -60, kYawMax = 60;
    static constexpr int kPitchMin = 0, kPitchMax = 30;  // 0 = level (raw 620), + = up
    static constexpr int kLevelPitch = 0;

    static HeadServo& GetInstance() { static HeadServo s; return s; }
    void Initialize(i2c_master_bus_handle_t i2c_bus);
    void RegisterMcpTools();

    // Queue a move; speed in deg/s (clamped 10..90). Returns false if queue full / faulted.
    bool MoveTo(int yaw, int pitch, int speed_dps = 40);
    bool Look(const char* dir);
    bool Nod();
    // Subtle mood gesture around the current pose; kind 0=hold offset, 1=dip/return, 2=wiggle. Short tracker pause.
    void Gesture(int dyaw, int dpitch, int kind);
    bool Shake();
    // Smooth (eased) named gesture: nod, shake, tilt, happy, sad, think, surprise, dizzy, bow, sleepy, love
    bool GestureNamed(const char* name);
    // Stock-style homing: torque on, sweep L/R/up, center, settle, verify read-back and correct. Runs on servo task.
    void RequestHome() { home_req_ = true; }
    void SetAutoMotion(bool on);
    bool AutoMotion() const { return auto_motion_; }
    void SetSleeping(bool s) { sleeping_ = s; }
    bool Sleeping() const { return sleeping_; }
    // Face tracking: queue a small corrective move and suppress idle glances for a while
    bool Track(int yaw, int pitch, int speed_dps);
    void NoteFace();
    bool FaceActive() const;
    bool ManualHold() const;
    // true while a move is in progress (plus settle time) -> camera frames are blurred/stale
    bool Busy() const;
    int Yaw() const { return cur_yaw_; }
    int Pitch() const { return cur_pitch_; }
    bool Ok() const { return ok_ && !fault_; }
    i2c_master_dev_handle_t Py32() const { return py32_; }

private:
    struct Cmd { int16_t yaw, pitch, speed; int16_t ease = 0; };
    HeadServo() = default;
    bool PowerEnable(bool on);
    bool WritePos(uint8_t id, int pos, int time_ms);
    int ReadPos(uint8_t id);
    void Task();
    void Execute(const Cmd& c);
    void AutoTick();
    void Home(bool sweep);
    bool MoveAxis(uint8_t id, int from_deg, int to_deg, int speed);
    bool MoveAxisEased(uint8_t id, int from_deg, int to_deg, int speed);
    bool QueueSeq(const int (*pts)[2], int n, int speed);

    i2c_master_dev_handle_t py32_ = nullptr;
    QueueHandle_t queue_ = nullptr;
    std::atomic<int> cur_yaw_{0}, cur_pitch_{0};
    std::atomic<bool> auto_motion_{HEAD_AUTO_MOTION != 0};
    std::atomic<bool> sleeping_{false};
    bool ok_ = false;
    std::atomic<bool> fault_{false};
    int consecutive_fail_ = 0;
    bool power_cycled_ = false;
    int64_t next_glance_us_ = 0;
    int64_t next_nod_us_ = 0;
    std::atomic<int64_t> face_until_us_{0};
    std::atomic<int64_t> manual_until_us_{0};
    std::atomic<int64_t> busy_until_us_{0};
    std::atomic<bool> home_req_{false};
    bool homed_ = false;
    std::atomic<int> last_home_yaw_{-1}, last_home_pitch_{-1};
};
