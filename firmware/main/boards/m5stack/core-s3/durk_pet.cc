#include "durk_pet.h"
#include "head_servo.h"
#include "durk_sd.h"
#include "application.h"
#include "board.h"
#include "display.h"
#include "mcp_server.h"
#include "settings.h"
#include "i2c_device.h"
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/idf_additions.h>
#include <esp_heap_caps.h>
#include <atomic>
#include <cmath>
#include <cstring>
#include "../../../../managed_components/espressif__bmi270_sensor/firmware/bmi270_image.h"

#define TAG "DurkPet"

namespace durk_pet {
namespace {
class Bmi270 : public I2cDevice {
public:
    Bmi270(i2c_master_bus_handle_t b) : I2cDevice(b, 0x69) {}
    uint8_t R(uint8_t r) { return ReadReg(r); }
    void W(uint8_t r, uint8_t v) { WriteReg(r, v); }
    void Rn(uint8_t r, uint8_t* b, size_t n) { ReadRegs(r, b, n); }
    void Wn(uint8_t r, const uint8_t* b, size_t n) { WriteRegs(r, b, n); }
};
Bmi270* g_imu = nullptr;
std::atomic<bool> g_imu_ok{false};
std::atomic<bool> g_enabled{true}, g_kid{false}, g_pet_flag{false};
int64_t g_last_react_us = 0;
int64_t now_us() { return esp_timer_get_time(); }

bool ImuInit(i2c_master_bus_handle_t bus) {
    g_imu = new Bmi270(bus);
    uint8_t id = g_imu->R(0x00);
    if (id != 0x24) { ESP_LOGW(TAG, "BMI270 not found (id 0x%02X); shake/spin off", id); return false; }
    g_imu->W(0x7C, 0x00);  // PWR_CONF: adv power save off
    vTaskDelay(pdMS_TO_TICKS(2));
    g_imu->W(0x59, 0x00);  // INIT_CTRL start
    const size_t chunk = 32;
    for (size_t i = 0; i < sizeof(bmi270_config_file); i += chunk) {
        g_imu->W(0x5B, (uint8_t)((i / 2) & 0x0F));
        g_imu->W(0x5C, (uint8_t)((i / 2) >> 4));
        size_t n = sizeof(bmi270_config_file) - i < chunk ? sizeof(bmi270_config_file) - i : chunk;
        g_imu->Wn(0x5E, bmi270_config_file + i, n);
    }
    g_imu->W(0x59, 0x01);
    vTaskDelay(pdMS_TO_TICKS(30));
    uint8_t st = g_imu->R(0x21) & 0x0F;
    if (st != 0x01) { ESP_LOGW(TAG, "BMI270 init status 0x%02X; shake/spin off", st); return false; }
    g_imu->W(0x40, 0xA8);  // ACC 100Hz
    g_imu->W(0x41, 0x02);  // +-8g -> 4096 LSB/g
    g_imu->W(0x42, 0xA9);  // GYR 200Hz
    g_imu->W(0x43, 0x00);  // +-2000 dps -> 16.4 LSB/dps
    g_imu->W(0x7D, 0x06);  // acc + gyr on
    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_LOGI(TAG, "BMI270 ready");
    return true;
}

bool CanReact() {
    if (!g_enabled) return false;
    auto st = Application::GetInstance().GetDeviceState();
    if (st != kDeviceStateIdle) return false;           // never interrupt listening/speaking
    if (now_us() - g_last_react_us < 4000000) return false;
    return true;
}

void React(const char* emotion, const char* gesture, const char* sfx) {
    g_last_react_us = now_us();
    ESP_LOGI(TAG, "react %s", gesture);
    auto disp = Board::GetInstance().GetDisplay();
    if (disp) disp->SetEmotion(emotion);
    HeadServo::GetInstance().GestureNamed(gesture);
    if (sfx && durk_sd::HasSfx(sfx)) durk_sd::PlaySfx(sfx);
    vTaskDelay(pdMS_TO_TICKS(3500));
    if (disp && Application::GetInstance().GetDeviceState() == kDeviceStateIdle) disp->SetEmotion("neutral");
}

void Task(void* arg) {
    // Wait until audio codec / servos finish their I2C init: an 8 KB IMU config burst during
    // boot made AW88298 init fail (assert -> reboot).
    vTaskDelay(pdMS_TO_TICKS(8000));
    bool imu = ImuInit((i2c_master_bus_handle_t)arg);
    g_imu_ok = imu;
    int shake_hits = 0; int64_t shake_win = 0;
    int spin_ms = 0;
    uint8_t b[12];
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(40));
        if (g_pet_flag.exchange(false) && CanReact()) {
            React(g_kid ? "loving" : "happy", g_kid ? "love" : "happy", "pet");
            continue;
        }
        if (!imu) continue;
        auto& head = HeadServo::GetInstance();
        if (head.Busy()) { shake_hits = 0; spin_ms = 0; continue; }  // our own head motion moves the IMU
        g_imu->Rn(0x0C, b, 12);
        float ax = (int16_t)(b[0] | b[1] << 8) / 4096.f, ay = (int16_t)(b[2] | b[3] << 8) / 4096.f,
              az = (int16_t)(b[4] | b[5] << 8) / 4096.f;
        float gx = (int16_t)(b[6] | b[7] << 8) / 16.4f, gy = (int16_t)(b[8] | b[9] << 8) / 16.4f,
              gz = (int16_t)(b[10] | b[11] << 8) / 16.4f;
        float amag = sqrtf(ax * ax + ay * ay + az * az);
        if (amag < 0.05f) continue;  // bad read
        int64_t t = now_us();
        if (fabsf(amag - 1.0f) > 0.9f) {
            if (t - shake_win > 1200000) { shake_win = t; shake_hits = 0; }
            shake_hits++;
        }
        float rot = fmaxf(fabsf(gz), fmaxf(fabsf(gx), fabsf(gy)));
        spin_ms = rot > 180.f ? spin_ms + 40 : 0;
        if (shake_hits >= 4 && CanReact()) {
            shake_hits = 0;
            if (g_kid) React("laughing", "happy", "laugh"); else React("shocked", "surprise", "shake");
        } else if (spin_ms >= 600 && CanReact()) {
            spin_ms = 0;
            React("confused", "dizzy", "dizzy");
        }
    }
}
}  // namespace

void Init(i2c_master_bus_handle_t bus) {
    Settings s("pet", false);
    g_enabled = s.GetBool("on", true);
    g_kid = s.GetBool("kid", false);
    // PSRAM stack (internal RAM is tight); task does I2C + UI only, never flash writes.
    xTaskCreateWithCaps(Task, "durk_pet", 4096, bus, 2, nullptr, MALLOC_CAP_SPIRAM);
}

void OnPet() { g_pet_flag = true; }
bool KidMode() { return g_kid; }

void RegisterMcpTools() {
    auto& mcp = McpServer::GetInstance();
    mcp.AddTool("robot.set_pet_mode",
                "Turn pet mode on/off (robot reacts when petted on the screen, shaken, or spun).",
                PropertyList({Property("on", kPropertyTypeBoolean)}),
                [](const PropertyList& p) -> ReturnValue {
                    g_enabled = p["on"].value<bool>();
                    Settings s("pet", true); s.SetBool("on", g_enabled);
                    return true;
                });
    mcp.AddTool("robot.set_kid_mode",
                "Turn kid mode on/off. While kid mode is ON you must talk like a gentle, playful friend for a young child: "
                "short simple words, no scary, violent or grown-up topics, lots of encouragement. Check robot.get_modes at the start of a chat.",
                PropertyList({Property("on", kPropertyTypeBoolean)}),
                [](const PropertyList& p) -> ReturnValue {
                    g_kid = p["on"].value<bool>();
                    Settings s("pet", true); s.SetBool("kid", g_kid);
                    if (g_kid) HeadServo::GetInstance().GestureNamed("happy");
                    return g_kid ? std::string("Kid mode ON. Speak simply and gently for a young child until it is turned off.")
                                 : std::string("Kid mode OFF. Back to normal.");
                });
    mcp.AddTool("robot.get_modes", "Get pet mode / kid mode state.", PropertyList(),
                [](const PropertyList&) -> ReturnValue {
                    return std::string("{\"pet_mode\":") + (g_enabled ? "true" : "false") + ",\"kid_mode\":" + (g_kid ? "true" : "false") +
                           ",\"imu\":" + (g_imu_ok ? "true" : "false") + "}";
                });
}
}  // namespace durk_pet
