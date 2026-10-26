#include "body_leds.h"
// Stack-chan style face (geometry ported from meganetaaan/m5stack-avatar, MIT License,
// Copyright (c) 2018 Shinya Ishikawa). Drawn with LVGL primitives, white on black.
#include "stackchan_display.h"
#include "durk_sd.h"
#include "mcp_server.h"
#include "settings.h"
#include <cJSON.h>
#include <esp_log.h>
#include "assets/lang_config.h"
#include <esp_random.h>
#include <cstring>
#include <cstdlib>
#include <atomic>
#include <dirent.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/idf_additions.h>

extern std::atomic<uint16_t> g_audio_output_level;  // audio_service.cc

namespace {
constexpr int kBrowX[2] = {96, 230};
constexpr int kBrowY[2] = {67, 72};
constexpr int kTickMs = 33;  // ~30 fps, matches LVGL refresh period
const lv_color_t kFg = lv_color_white();
const lv_color_t kBg = lv_color_black();

lv_obj_t* MakeRect(lv_obj_t* parent, lv_color_t c) {
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(o, c, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}
}  // namespace

StackchanDisplay::~StackchanDisplay() {
    if (timer_) lv_timer_delete(timer_);
}

void StackchanDisplay::SetupUI() {
    SpiLcdDisplay::SetupUI();
    DisplayLockGuard lock(this);
    if (emoji_box_) lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
    // Opaque bars: no alpha blending over the face area
    if (top_bar_) lv_obj_set_style_bg_opa(top_bar_, LV_OPA_COVER, 0);
    if (bottom_bar_) lv_obj_set_style_bg_opa(bottom_bar_, LV_OPA_COVER, 0);

    auto screen = lv_screen_active();
    face_ = MakeRect(screen, kBg);
    lv_obj_set_size(face_, width_, height_);
    lv_obj_set_pos(face_, 0, 0);
    // Directly above the background container, below status/subtitle bars
    lv_obj_move_to_index(face_, 1);

    for (int i = 0; i < 2; i++) {
        eye_[i] = MakeRect(face_, kFg);
        lid_[i] = MakeRect(face_, kBg);
        lv_obj_add_flag(lid_[i], LV_OBJ_FLAG_HIDDEN);
        brow_[i] = lv_line_create(face_);
        lv_obj_set_style_line_color(brow_[i], kFg, 0);
        lv_obj_set_style_line_width(brow_[i], 4, 0);
        lv_obj_set_style_line_rounded(brow_[i], true, 0);
        lv_obj_add_flag(brow_[i], LV_OBJ_FLAG_HIDDEN);
    }
    mouth_ = MakeRect(face_, kFg);
    {
        Settings st("face", false);
        std::string saved = st.GetString("theme", "classic");
        if (saved != "classic") SetFaceThemeFromJsonLocked(saved);
    }
    ApplyThemeLocked();
    Render();
    timer_ = lv_timer_create(TimerCb, kTickMs, this);
    // Backdrop images live on the SD card, which mounts after the UI: load it a few seconds later.
    if (!th_.bg_image.empty())
        xTaskCreateWithCaps(DeferredBgTask, "facebg", 4096, this, 1, nullptr, MALLOC_CAP_SPIRAM);
}

void StackchanDisplay::DeferredBgTask(void* arg) {
    auto* self = static_cast<StackchanDisplay*>(arg);
    vTaskDelay(pdMS_TO_TICKS(4000));
    std::string name;
    {
        DisplayLockGuard lock(self);
        name = self->th_.name;
    }
    self->SetFaceTheme(name, false);  // reload only; already saved
    vTaskSuspend(nullptr);  // PSRAM-stack task: park instead of self-delete
}

void StackchanDisplay::TimerCb(lv_timer_t* t) {
    static_cast<StackchanDisplay*>(lv_timer_get_user_data(t))->Tick();
}

void StackchanDisplay::Tick() {
    // Blink / wink scheduling (m5stack-avatar: open 2.5-4.5s, closed ~300ms)
    if (blink_ticks_ > 0) {
        if (--blink_ticks_ == 0) {
            wink_eye_ = -1;
            next_blink_ = (2500 + esp_random() % 2000) / kTickMs;
        }
    } else if (--next_blink_ <= 0) {
        blink_ticks_ = 300 / kTickMs;
        wink_eye_ = (esp_random() % 8 == 0) ? (int)(esp_random() % 2) : -1;  // occasional wink
    }
    // Lip sync from real output loudness (RMS of PCM being written to the codec)
    {
        float target = 0.0f;
        if (speaking_) {
            float lvl = g_audio_output_level.load(std::memory_order_relaxed);
            target = (lvl - 300.0f) / 5000.0f;  // noise floor .. loud speech
            if (target < 0) target = 0;
            if (target > 1) target = 1;
        }
        // fast attack, slower release
        // Smooth per-frame interpolation toward the audio level (fast attack, slower release)
        mouth_open_ += (target - mouth_open_) * (target > mouth_open_ ? 0.45f : 0.25f);
        if (mouth_open_ < 0.03f) mouth_open_ = 0;
    }
    if (th_.twinkle && nstars_ > 0 && (++tick_count_ % 4) == 0) {
        lv_obj_t* s = star_[esp_random() % nstars_];
        lv_obj_set_style_bg_opa(s, (lv_opa_t)(60 + esp_random() % 196), 0);
    }
    Render();
}

void StackchanDisplay::Render() {
    if (!face_) return;
    for (int i = 0; i < 2; i++) {
        bool closed = (blink_ticks_ > 0 && (wink_eye_ < 0 || wink_eye_ == i)) || expr_ == kSleepy;
        int x = th_.eye_x[i], y = th_.eye_y[i];
        int st = closed ? 1 : (expr_ == kHappy ? 2 : 0);
        if (th_.single_eye && i == 1) { lv_obj_add_flag(eye_[1], LV_OBJ_FLAG_HIDDEN); lv_obj_add_flag(lid_[1], LV_OBJ_FLAG_HIDDEN); st = 3; }
        else if (i == 1) lv_obj_remove_flag(eye_[1], LV_OBJ_FLAG_HIDDEN);
        if (st != eye_state_[i] && st != 3) {
            eye_state_[i] = st;
            if (closed) {
                lv_obj_set_style_radius(eye_[i], 0, 0);
                lv_obj_set_size(eye_[i], th_.eye_r * 2, 4);
                lv_obj_set_pos(eye_[i], x - th_.eye_r, y - 2);
            } else {
                lv_obj_set_style_radius(eye_[i], th_.square_eyes ? 2 : LV_RADIUS_CIRCLE, 0);
                lv_obj_set_size(eye_[i], th_.eye_r * 2, th_.eye_r * 2);
                lv_obj_set_pos(eye_[i], x - th_.eye_r, y - th_.eye_r);
            }
            if (st == 2 && Fancy()) {
                // No solid lid over a picture: squash the eye into an upper crescent-ish pill instead
                int hh = th_.eye_r / 2 + 2;
                lv_obj_set_style_radius(eye_[i], LV_RADIUS_CIRCLE, 0);
                lv_obj_set_size(eye_[i], th_.eye_r * 2, hh);
                lv_obj_set_pos(eye_[i], x - th_.eye_r, y - th_.eye_r + 2);
                lv_obj_add_flag(lid_[i], LV_OBJ_FLAG_HIDDEN);
            } else if (st == 2) {
                lv_obj_set_style_radius(lid_[i], LV_RADIUS_CIRCLE, 0);
                lv_obj_set_size(lid_[i], th_.eye_r * 2, th_.eye_r * 2);
                lv_obj_set_pos(lid_[i], x - th_.eye_r, y - th_.eye_r + th_.eye_r / 2 + 2);
                lv_obj_remove_flag(lid_[i], LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(lid_[i], LV_OBJ_FLAG_HIDDEN);
            }
        }
        // Eyebrows: only visible for angry / sad / doubt (as in m5stack-avatar)
        int bx = th_.eye_x[i] + (kBrowX[i] - 90 - (i ? 140 : 0)), by = th_.eye_y[i] - th_.eye_r - 18 + (kBrowY[i] - 67 - (i ? 5 : 0));
        int dy = 0;
        bool show = true;
        // a = inner end lifts/drops; left eye inner end is right side
        int inner_sign = (i == 0) ? 1 : -1;
        switch (expr_) {
            case kAngry: dy = 6; break;     // inner end down
            case kSad: dy = -6; break;      // inner end up
            case kDoubt: dy = (i == 0) ? 0 : -5; break;
            default: show = false; break;
        }
        if (th_.brow_tilt) { show = true; dy += th_.brow_tilt; }  // e.g. angry-bot: permanent scowl
        if (th_.single_eye && i == 1) show = false;
        int bst = show ? (int)expr_ + 100 * th_.brow_tilt : -1;
        if (bst == brow_state_[i]) continue;
        brow_state_[i] = bst;
        if (show) {
            int lx = bx - th_.brow_w / 2, rx = bx + th_.brow_w / 2;
            int ly = by, ry = by;
            if (inner_sign > 0) ry += dy; else ly += dy;
            brow_pts_[i][0] = {(lv_value_precise_t)lx, (lv_value_precise_t)ly};
            brow_pts_[i][1] = {(lv_value_precise_t)rx, (lv_value_precise_t)ry};
            lv_line_set_points(brow_[i], brow_pts_[i], 2);
            lv_obj_remove_flag(brow_[i], LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(brow_[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    float open = mouth_open_;
    if (!speaking_) {
        if (expr_ == kHappy) open = 0.15f;
        else if (expr_ == kAngry) open = 0.0f;
    }
    int h = th_.mouth_min_h + (int)((th_.mouth_max_h - th_.mouth_min_h) * open);
    int w = th_.mouth_min_w + (int)((th_.mouth_max_w - th_.mouth_min_w) * (1.0f - open));
    if (expr_ == kSad && !speaking_) w = th_.mouth_min_w;
    if (w == mouth_w_ && h == mouth_h_) return;
    // Only resize when the change is visible (>=3 px) to avoid redraw churn
    if (mouth_w_ >= 0 && abs(w - mouth_w_) < 2 && abs(h - mouth_h_) < 2 && h != th_.mouth_min_h) return;
    mouth_w_ = w; mouth_h_ = h;
    lv_obj_set_size(mouth_, w, h);
    lv_obj_set_style_radius(mouth_, h > 8 ? 6 : 0, 0);
    lv_obj_set_pos(mouth_, th_.mouth_cx - w / 2, th_.mouth_cy - h / 2);
}

void StackchanDisplay::SetEmotion(const char* emotion) {
    if (emotion == nullptr) return;
    BodyLeds::GetInstance().SetMood(emotion);
    static const struct { const char* name; Expr e; } kMap[] = {
        {"happy", kHappy}, {"laughing", kHappy}, {"funny", kHappy}, {"loving", kHappy},
        {"winking", kHappy}, {"cool", kHappy}, {"delicious", kHappy}, {"kissy", kHappy},
        {"confident", kHappy}, {"silly", kHappy},
        {"sad", kSad}, {"crying", kSad}, {"embarrassed", kSad},
        {"angry", kAngry},
        {"sleepy", kSleepy}, {"relaxed", kSleepy},
        {"confused", kDoubt}, {"thinking", kDoubt}, {"surprised", kDoubt}, {"shocked", kDoubt},
    };
    Expr e = kNeutral;
    for (auto& m : kMap) {
        if (strcmp(emotion, m.name) == 0) { e = m.e; break; }
    }
    DisplayLockGuard lock(this);
    expr_ = e;
    if (strcmp(emotion, "winking") == 0) {
        blink_ticks_ = 600 / kTickMs;
        wink_eye_ = 1;
    }
    Render();
}

// Durk: base class re-shows emoji_box_ (centered robot glyph) when a camera preview ends;
// keep it hidden so it never sits on top of the face.
void StackchanDisplay::SetPreviewImage(std::unique_ptr<LvglImage> image) {
    SpiLcdDisplay::SetPreviewImage(std::move(image));
    DisplayLockGuard lock(this);
    if (emoji_box_) lv_obj_add_flag(emoji_box_, LV_OBJ_FLAG_HIDDEN);
}

void StackchanDisplay::SetStatus(const char* status) {
    SpiLcdDisplay::SetStatus(status);
    speaking_ = status && strcmp(status, Lang::Strings::SPEAKING) == 0;
}

void StackchanDisplay::SetTheme(Theme* theme) {
    SpiLcdDisplay::SetTheme(theme);
    DisplayLockGuard lock(this);
    if (top_bar_) lv_obj_set_style_bg_opa(top_bar_, LV_OPA_COVER, 0);
    if (bottom_bar_) lv_obj_set_style_bg_opa(bottom_bar_, LV_OPA_COVER, 0);
}

// ---------------- Durk face themes ----------------
namespace {
const char* kBuiltinThemes[][2] = {
    {"classic", R"({"name":"classic"})"},
    {"cyclops", R"({"name":"cyclops","fg":"#7CFFB2","bg":"#001008","eye_r":26,"eye_x":[160,160],"eye_y":[92,92],"single_eye":true,"mouth_cx":160,"mouth_cy":170,"mouth_min_w":40,"mouth_max_w":70,"mouth_min_h":4,"mouth_max_h":40,"brow_w":60})"},
    {"angry-bot", R"({"name":"angry-bot","fg":"#FF3B1F","bg":"#000000","eye_r":12,"eye_x":[95,225],"eye_y":[100,100],"square_eyes":true,"mouth_cx":160,"mouth_cy":160,"mouth_min_w":70,"mouth_max_w":100,"mouth_min_h":6,"mouth_max_h":50,"brow_w":44,"brow_tilt":10})"},
};
uint32_t ParseColor(const cJSON* j, uint32_t def) {
    if (!cJSON_IsString(j) || !j->valuestring) return def;
    const char* s = j->valuestring;
    if (*s == '#') s++;
    return (uint32_t)strtoul(s, nullptr, 16);
}
int Clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
bool ParseTheme(const std::string& json, StackchanDisplay::FaceTheme& t) {
    cJSON* r = cJSON_Parse(json.c_str());
    if (!r) return false;
    StackchanDisplay::FaceTheme d;  // start from classic defaults
    auto gi = [&](const char* k, int def, int lo, int hi) {
        cJSON* v = cJSON_GetObjectItem(r, k);
        return cJSON_IsNumber(v) ? Clamp(v->valueint, lo, hi) : def;
    };
    auto gb = [&](const char* k, bool def) {
        cJSON* v = cJSON_GetObjectItem(r, k);
        return cJSON_IsBool(v) ? (bool)cJSON_IsTrue(v) : def;
    };
    cJSON* n = cJSON_GetObjectItem(r, "name");
    if (cJSON_IsString(n)) d.name = n->valuestring;
    d.fg = ParseColor(cJSON_GetObjectItem(r, "fg"), d.fg);
    d.bg = ParseColor(cJSON_GetObjectItem(r, "bg"), d.bg);
    d.eye_r = gi("eye_r", d.eye_r, 3, 40);
    for (const char* k : {"eye_x", "eye_y"}) {
        cJSON* a = cJSON_GetObjectItem(r, k);
        int* dst = k[4] == 'x' ? d.eye_x : d.eye_y;
        if (cJSON_IsArray(a) && cJSON_GetArraySize(a) == 2)
            for (int i = 0; i < 2; i++) dst[i] = Clamp(cJSON_GetArrayItem(a, i)->valueint, 20, k[4] == 'x' ? 300 : 200);
    }
    d.square_eyes = gb("square_eyes", d.square_eyes);
    d.single_eye = gb("single_eye", d.single_eye);
    d.mouth_cx = gi("mouth_cx", d.mouth_cx, 40, 280);
    d.mouth_cy = gi("mouth_cy", d.mouth_cy, 100, 220);
    d.mouth_min_w = gi("mouth_min_w", d.mouth_min_w, 10, 200);
    d.mouth_max_w = gi("mouth_max_w", d.mouth_max_w, 10, 200);
    d.mouth_min_h = gi("mouth_min_h", d.mouth_min_h, 2, 80);
    d.mouth_max_h = gi("mouth_max_h", d.mouth_max_h, 2, 80);
    d.brow_w = gi("brow_w", d.brow_w, 8, 100);
    d.brow_tilt = gi("brow_tilt", d.brow_tilt, -20, 20);
    d.stars = gi("stars", 0, 0, StackchanDisplay::kMaxStarsPublic);
    d.twinkle = gb("twinkle", false);
    cJSON* bi = cJSON_GetObjectItem(r, "bg_image");
    if (cJSON_IsString(bi) && bi->valuestring && !strchr(bi->valuestring, '/') && !strstr(bi->valuestring, ".."))
        d.bg_image = bi->valuestring;
    cJSON_Delete(r);
    t = d;
    return true;
}
}  // namespace

bool StackchanDisplay::SetFaceThemeFromJsonLocked(const std::string& name) {
    std::string json;
    for (auto& b : kBuiltinThemes) if (name == b[0]) json = b[1];
    // SD file overrides / adds themes (read outside the display lock is preferred; small file, rare)
    if (durk_sd::Mounted() && name.find('/') == std::string::npos) {
        std::string f = durk_sd::ReadText(std::string(durk_sd::kMount) + "/faces/" + name + ".json");
        if (!f.empty()) json = f;
    }
    FaceTheme t;
    if (json.empty() || !ParseTheme(json, t)) return false;
    t.name = name;
    th_ = t;
    return true;
}

void StackchanDisplay::ApplyThemeLocked() {
    if (!face_) return;
    lv_color_t fg = lv_color_hex(th_.fg), bg = lv_color_hex(th_.bg);
    lv_obj_set_style_bg_color(face_, bg, 0);
    for (int i = 0; i < 2; i++) {
        lv_obj_set_style_bg_color(eye_[i], fg, 0);
        lv_obj_set_style_bg_color(lid_[i], bg, 0);
        lv_obj_set_style_line_color(brow_[i], fg, 0);
        eye_state_[i] = -1;
        brow_state_[i] = -2;
    }
    lv_obj_set_style_bg_color(mouth_, fg, 0);
    mouth_w_ = mouth_h_ = -1;
    BuildStarsLocked();
    if (th_.bg_image.empty()) SetBackdropLocked(nullptr);
}

void StackchanDisplay::BuildStarsLocked() {
    for (int i = 0; i < nstars_; i++) lv_obj_delete(star_[i]);
    nstars_ = 0;
    uint32_t seed = 12345;  // fixed layout so the sky doesn't jump on theme reload
    auto rnd = [&seed]() { seed = seed * 1103515245u + 12345u; return (seed >> 16) & 0x7fff; };
    for (int i = 0; i < th_.stars && i < kMaxStars; i++) {
        int sz = (rnd() % 5 == 0) ? 2 : 1;
        lv_obj_t* o = MakeRect(face_, lv_color_hex((rnd() % 3 == 0) ? 0xBFD8FF : 0xFFFFFF));
        lv_obj_set_size(o, sz, sz);
        lv_obj_set_pos(o, rnd() % width_, rnd() % height_);
        lv_obj_set_style_bg_opa(o, (lv_opa_t)(120 + rnd() % 136), 0);
        lv_obj_move_to_index(o, 0);  // behind eyes / mouth
        star_[nstars_++] = o;
    }
    if (bg_img_) lv_obj_move_to_index(bg_img_, 0);
}

void StackchanDisplay::SetBackdropLocked(uint8_t* buf) {
    uint8_t* old = bg_buf_;
    if (buf) {
        if (!bg_img_) {
            bg_img_ = lv_image_create(face_);
            lv_obj_remove_flag(bg_img_, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_pos(bg_img_, 0, 0);
        }
        bg_dsc_ = {};
        bg_dsc_.header.magic = LV_IMAGE_HEADER_MAGIC;
        bg_dsc_.header.cf = LV_COLOR_FORMAT_RGB565;
        bg_dsc_.header.w = 320;
        bg_dsc_.header.h = 240;
        bg_dsc_.header.stride = 640;
        bg_dsc_.data_size = 320 * 240 * 2;
        bg_dsc_.data = buf;
        lv_image_set_src(bg_img_, &bg_dsc_);
        lv_obj_remove_flag(bg_img_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_to_index(bg_img_, 0);
        bg_buf_ = buf;
    } else {
        if (bg_img_) {
            lv_obj_add_flag(bg_img_, LV_OBJ_FLAG_HIDDEN);
            lv_image_set_src(bg_img_, nullptr);
        }
        bg_buf_ = nullptr;
    }
    if (old && old != buf) heap_caps_free(old);
    for (int i = 0; i < 2; i++) eye_state_[i] = -1;  // re-pick lid vs crescent
}

bool StackchanDisplay::SetFaceTheme(const std::string& name, bool persist) {
    // Read the SD file (if any) before taking the display lock: SD traffic itself takes that lock.
    std::string sdjson;
    if (durk_sd::Mounted() && name.find('/') == std::string::npos)
        sdjson = durk_sd::ReadText(std::string(durk_sd::kMount) + "/faces/" + name + ".json");
    std::string json = sdjson;
    if (json.empty()) for (auto& b : kBuiltinThemes) if (name == b[0]) json = b[1];
    FaceTheme t;
    if (json.empty() || !ParseTheme(json, t)) return false;
    t.name = name;
    uint8_t* bg = nullptr;
    if (!t.bg_image.empty()) {
        bg = durk_sd::ReadExactPsram(std::string(durk_sd::kMount) + "/faces/" + t.bg_image, 320 * 240 * 2);
        if (!bg) ESP_LOGW("StackchanDisplay", "backdrop %s missing/wrong size - using solid bg", t.bg_image.c_str());
    }
    {
        DisplayLockGuard lock(this);
        th_ = t;
        ApplyThemeLocked();
        if (bg) SetBackdropLocked(bg); else SetBackdropLocked(nullptr);
        Render();
    }
    if (persist) {  // never from PSRAM-stack tasks: NVS flash writes need an internal-RAM stack
        Settings st("face", true);
        st.SetString("theme", name);
    }
    ESP_LOGI("StackchanDisplay", "face theme -> %s", name.c_str());
    return true;
}

void StackchanDisplay::RegisterMcpTools() {
    auto& mcp = McpServer::GetInstance();
    mcp.AddTool("robot.set_face_theme",
                "Change the robot's face style. Built-in: classic (default white Stack-chan), cyclops (one big green "
                "eye), angry-bot (red square eyes, scowl), galaxy (starry space background; 'galaxy mode'). More can be added as JSON files in /faces on the SD card.",
                PropertyList({Property("name", kPropertyTypeString)}),
                [this](const PropertyList& p) -> ReturnValue { return SetFaceTheme(p["name"].value<std::string>()); });
    mcp.AddTool("robot.list_face_themes", "List available face themes and the current one.", PropertyList(),
                [this](const PropertyList&) -> ReturnValue {
                    std::string s = "{\"current\":\"" + th_.name + "\",\"themes\":[\"classic\",\"cyclops\",\"angry-bot\"";
                    if (durk_sd::Mounted()) {
                        DIR* d = opendir("/sdcard/faces");
                        if (d) {
                            while (auto* e = readdir(d)) {
                                std::string n = e->d_name;
                                if (n.size() > 5 && n.substr(n.size() - 5) == ".json") {
                                    n = n.substr(0, n.size() - 5);
                                    if (n != "classic" && n != "cyclops" && n != "angry-bot") s += ",\"" + n + "\"";
                                }
                            }
                            closedir(d);
                        }
                    }
                    return s + "]}";
                });
}
