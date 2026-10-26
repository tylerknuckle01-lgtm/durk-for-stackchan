// Stack-chan style face (geometry ported from meganetaaan/m5stack-avatar, MIT)
#pragma once
#include "display/lcd_display.h"
#include <string>

class StackchanDisplay : public SpiLcdDisplay {
public:
    using SpiLcdDisplay::SpiLcdDisplay;
    ~StackchanDisplay();
    void SetupUI() override;
    void SetEmotion(const char* emotion) override;
    void SetStatus(const char* status) override;
    void SetTheme(Theme* theme) override;
    void SetPreviewImage(std::unique_ptr<LvglImage> image) override;
    // Durk face themes: built-in classic/cyclops/angry-bot, or /sdcard/faces/<name>.json
    bool SetFaceTheme(const std::string& name, bool persist = true);
    std::string FaceThemeName() const { return th_.name; }
    void RegisterMcpTools();
    static constexpr int kMaxStarsPublic = 60;
    struct FaceTheme {
        std::string name = "classic";
        uint32_t fg = 0xFFFFFF, bg = 0x000000;
        int eye_r = 8, eye_x[2] = {90, 230}, eye_y[2] = {93, 96};
        bool square_eyes = false, single_eye = false;
        int mouth_cx = 163, mouth_cy = 148, mouth_min_w = 50, mouth_max_w = 90, mouth_min_h = 4, mouth_max_h = 60;
        int brow_w = 32, brow_tilt = 0;
        std::string bg_image;      // /sdcard/faces/<file>: raw RGB565 320x240 (153600 bytes)
        int stars = 0;             // code-drawn starfield (0-60)
        bool twinkle = false;
    };

private:
    enum Expr { kNeutral, kHappy, kSad, kAngry, kSleepy, kDoubt };
    lv_obj_t* face_ = nullptr;
    lv_obj_t* eye_[2] = {};
    lv_obj_t* lid_[2] = {};   // black circle used for "happy" arc eyes
    lv_obj_t* brow_[2] = {};
    lv_point_precise_t brow_pts_[2][2] = {};
    lv_obj_t* mouth_ = nullptr;
    lv_timer_t* timer_ = nullptr;
    // Optional backdrop (galaxy themes). Only used when the theme asks for it.
    lv_obj_t* bg_img_ = nullptr;
    lv_image_dsc_t bg_dsc_ = {};
    uint8_t* bg_buf_ = nullptr;
    static constexpr int kMaxStars = 60;
    lv_obj_t* star_[kMaxStars] = {};
    int nstars_ = 0;
    bool Fancy() const { return bg_buf_ != nullptr || nstars_ > 0; }
    void SetBackdropLocked(uint8_t* buf);
    void BuildStarsLocked();
    static void DeferredBgTask(void* arg);
    Expr expr_ = kNeutral;
    FaceTheme th_;
    void ApplyThemeLocked();
    bool SetFaceThemeFromJsonLocked(const std::string& name);
    bool speaking_ = false;
    int blink_ticks_ = 0;     // remaining closed ticks
    int next_blink_ = 40;
    int wink_eye_ = -1;
    float mouth_open_ = 0.0f;
    int tick_count_ = 0;
    // Last applied geometry, so LVGL is only touched (and areas invalidated) on real changes
    int eye_state_[2] = {-1, -1};   // 0=open,1=closed,2=happy
    int brow_state_[2] = {-1, -1};  // expr id or -1 hidden
    int mouth_w_ = -1, mouth_h_ = -1;

    static void TimerCb(lv_timer_t* t);
    void Tick();
    void Render();
};
