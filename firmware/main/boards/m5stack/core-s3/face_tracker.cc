#include "face_tracker.h"
#include "head_servo.h"
#include "application.h"
#include "mcp_server.h"
#include "settings.h"
#include "esp_video.h"
#include "human_face_detect.hpp"

#include <esp_log.h>
#include <esp_timer.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/idf_additions.h>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <algorithm>

#define TAG "FaceTrack"

namespace {
constexpr float kHalfFovX = 30.0f;   // GC0308 ~60 deg horizontal
constexpr float kHalfFovY = 22.0f;
constexpr float kDeadX = 0.12f, kDeadY = 0.15f;  // fraction of half-frame
constexpr float kGain = 0.6f;
constexpr int kMaxStep = 8;          // deg per correction
constexpr int kPeriodMs = 300;
int64_t now_ms() { return esp_timer_get_time() / 1000; }
}

void FaceTracker::Start(EspVideo* camera) {
    camera_ = camera;
    if (!camera_) { ESP_LOGW(TAG, "no camera"); return; }
    {
        // v2: clear any auto-flip state saved by the first build (pitch is no longer tracked)
        Settings w("face", true);
        if (w.GetInt("cfg_ver", 0) < 2) {
            w.SetInt("inv_x", 1);
            w.SetInt("inv_y", 0);
            w.SetInt("cfg_ver", 2);
            ESP_LOGI(TAG, "reset saved calibration");
        }
    }
    Settings s("face", false);
    enabled_ = s.GetInt("enabled", 1) != 0;
    // First field test ran away to the limits on both axes => camera image is rotated 180 deg.
    invert_x_ = s.GetInt("inv_x", 1) != 0;
    invert_y_ = false;  // pitch is not tracked
    // Low priority, core 1, stack in PSRAM: must never starve audio/display
    // Low priority on core 1 so it never starves audio/display. Grid/frame buffers live in PSRAM.
    BaseType_t ok = xTaskCreatePinnedToCore([](void* a) { static_cast<FaceTracker*>(a)->Task(); },
                                            "face_track", 12288, this, 1, nullptr, 1);
    if (ok != pdPASS) { ESP_LOGE(TAG, "task create failed"); return; }
    ESP_LOGI(TAG, "started (enabled=%d inv_x=%d inv_y=%d)", (int)enabled_, (int)invert_x_, (int)invert_y_);
}

void FaceTracker::SetEnabled(bool on) {
    enabled_ = on;
    Settings s("face", true);
    s.SetInt("enabled", on ? 1 : 0);
    ESP_LOGI(TAG, "tracking %s", on ? "ON" : "OFF");
}

// Lightweight detector: skin-colour blobs on an 8x8-block grid, with cells that stay
// skin-coloured and motionless for a long time treated as background (walls, desk).
namespace {
constexpr int kCell = 8;
constexpr int kMaxGW = 40, kMaxGH = 30;
struct Grid {
    int gw = 0, gh = 0;
    uint8_t y[kMaxGW * kMaxGH], cb[kMaxGW * kMaxGH], cr[kMaxGW * kMaxGH];
    uint8_t prev_y[kMaxGW * kMaxGH];
    uint16_t still[kMaxGW * kMaxGH];  // consecutive frames skin & motionless
    uint8_t skin[kMaxGW * kMaxGH];
    int16_t label[kMaxGW * kMaxGH];
    int16_t stack[kMaxGW * kMaxGH];
};
inline void rgb_of(uint16_t px, int& r, int& g, int& b) {
    r = (px >> 11) << 3; g = ((px >> 5) & 0x3f) << 2; b = (px & 0x1f) << 3;
}
// Mean |green diff| between horizontal neighbours: the right byte order gives a smoother image.
int roughness(const uint8_t* d, int w, int h, bool be) {
    long acc = 0; int n = 0;
    for (int yy = h / 4; yy < h * 3 / 4; yy += 8) {
        const uint8_t* row = d + yy * w * 2;
        for (int xx = 0; xx + 1 < w; xx += 3) {
            uint16_t a = be ? (row[xx * 2] << 8 | row[xx * 2 + 1]) : (row[xx * 2 + 1] << 8 | row[xx * 2]);
            uint16_t b = be ? (row[xx * 2 + 2] << 8 | row[xx * 2 + 3]) : (row[xx * 2 + 3] << 8 | row[xx * 2 + 2]);
            acc += abs((int)((a >> 5) & 0x3f) - (int)((b >> 5) & 0x3f)); n++;
        }
    }
    return n ? (int)(acc * 100 / n) : 0;
}
}

namespace {
int roughness_y(const uint8_t* d, int w, int h, int off) {
    long acc = 0; int n = 0;
    for (int yy = h / 4; yy < h * 3 / 4; yy += 8) {
        const uint8_t* row = d + yy * w * 2;
        for (int xx = 0; xx + 3 < w * 2; xx += 6) { acc += abs((int)row[xx + off] - (int)row[xx + off + 2]); n++; }
    }
    return n ? (int)(acc * 100 / n) : 0;
}
}

void FaceTracker::Task() {
    vTaskDelay(pdMS_TO_TICKS(10000));  // let boot/network/audio settle
    HeadServo::GetInstance().MoveTo(0, HeadServo::kLevelPitch, 30);  // start from level
    Grid* g = (Grid*)heap_caps_calloc(1, sizeof(Grid), MALLOC_CAP_SPIRAM);
    uint8_t* copy = nullptr;
    size_t copy_cap = 0;
    int order = -1;  // -1 unknown, 0 LE, 1 BE
    int votes_be = 0, votes_le = 0;
    int frames = 0, faces = 0, work_ms = 0, last_blob = 0;
    int64_t stat_t = now_ms();
    bool locked = false;
    HumanFaceDetect* det = nullptr;
    int dl_hits = 0, dl_frames = 0, dl_ms = 0;
    float target_pitch = HeadServo::kLevelPitch;
    float target_world = 0, cand_world = 0;
    int cand_hits = 0;
    int64_t last_seen = 0;
    int64_t det_log_t = 0;
    constexpr int kLevelPitch = HeadServo::kLevelPitch;
    auto& app = Application::GetInstance();
    auto& head = HeadServo::GetInstance();
    if (!g) { ESP_LOGE(TAG, "no memory"); vTaskDelete(nullptr); return; }

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(kPeriodMs));
        auto st = app.GetDeviceState();
        bool active = enabled_ && !head.Sleeping() && (st == kDeviceStateIdle || st == kDeviceStateListening);
        if (!active) {
            if (now_ms() - stat_t >= 30000) {
                ESP_LOGI(TAG, "paused (enabled=%d sleeping=%d state=%d)", (int)enabled_, (int)head.Sleeping(), (int)st);
                stat_t = now_ms();
            }
            continue;
        }

        int w = 0, h = 0;
        uint32_t fmt = 0;
        bool got = camera_->GrabRaw([&](const uint8_t* d, size_t len, int fw, int fh, uint32_t f) {
            if (len > copy_cap) {
                if (copy) heap_caps_free(copy);
                copy = (uint8_t*)heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
                copy_cap = copy ? len : 0;
            }
            if (copy) { memcpy(copy, d, len); w = fw; h = fh; fmt = f; }
        });
        if (!got || !copy || w < kCell * 4 || h < kCell * 4) {
            if (now_ms() - stat_t >= 10000) { ESP_LOGW(TAG, "no frame (got=%d w=%d h=%d)", (int)got, w, h); stat_t = now_ms(); }
            continue;
        }
        bool yuv = (fmt == V4L2_PIX_FMT_UYVY || fmt == V4L2_PIX_FMT_YUYV);
        if (!yuv && fmt != V4L2_PIX_FMT_RGB565 && fmt != V4L2_PIX_FMT_RGB565X) {
            ESP_LOGW(TAG, "unsupported camera format 0x%08lx, tracking disabled", (unsigned long)fmt);
            enabled_ = false;
            continue;
        }
        int64_t t0 = now_ms();
        if (order < 0) {
            int le, be;
            if (yuv) { le = roughness_y(copy, w, h, 0); be = roughness_y(copy, w, h, 1); }  // luma at even / odd bytes
            else { le = roughness(copy, w, h, false); be = roughness(copy, w, h, true); }
            (be < le ? votes_be : votes_le)++;
            if (votes_be + votes_le >= 5) {
                order = votes_be > votes_le ? 1 : 0;
                ESP_LOGI(TAG, "camera %dx%d fmt=%s, order %d (votes %d/%d)", w, h, yuv ? "YUV422" : "RGB565", order, votes_le, votes_be);
            }
            continue;
        }
        g->gw = std::min(kMaxGW, w / kCell);
        g->gh = std::min(kMaxGH, h / kCell);
        int gw = g->gw, gh = g->gh, n = gw * gh;
        // 1) block means (sample 4 px per block) -> YCbCr, skin mask, motion
        for (int cy = 0; cy < gh; cy++) {
            for (int cx = 0; cx < gw; cx++) {
                int Y = 0, Cb = 0, Cr = 0;
                for (int k = 0; k < 4; k++) {
                    int px = (cx * kCell + 2 + (k & 1) * 4) & ~1, py = cy * kCell + 2 + (k >> 1) * 4;
                    const uint8_t* p = copy + (py * w + px) * 2;  // 4 bytes = 2 pixels
                    if (yuv) {
                        // order 1: U Y V Y (luma odd bytes); order 0: Y U Y V
                        int yy = order ? p[1] : p[0], u = order ? p[0] : p[1], v = order ? p[2] : p[3];
                        if (fmt == V4L2_PIX_FMT_YUYV && order == 0) { u = p[1]; v = p[3]; }
                        Y += yy; Cb += u; Cr += v;
                    } else {
                        uint16_t vv = order ? (p[0] << 8 | p[1]) : (p[1] << 8 | p[0]);
                        int r, gg, b; rgb_of(vv, r, gg, b);
                        Y += (77 * r + 150 * gg + 29 * b) >> 8;
                        Cb += 128 + ((-43 * r - 85 * gg + 128 * b) >> 8);
                        Cr += 128 + ((128 * r - 107 * gg - 21 * b) >> 8);
                    }
                }
                Y /= 4; Cb /= 4; Cr /= 4;
                int i = cy * gw + cx;
                bool sk = Y > 45 && Y < 240 && Cb >= 77 && Cb <= 127 && Cr >= 135 && Cr <= 175;
                bool moving = abs(Y - g->prev_y[i]) > 10;
                g->prev_y[i] = Y;
                if (sk && !moving) { if (g->still[i] < 65000) g->still[i]++; }
                else if (moving) g->still[i] = 0;
                // skin that has sat motionless for ~30 s is background
                g->skin[i] = sk && g->still[i] < 200;
                g->label[i] = 0;
            }
        }
        // 2) 4-connected skin blobs; keep face-shaped ones
        struct Blob { int sz; float x, y; int w, h; };
        Blob blobs[8]; int nb_found = 0; int lbl = 0; int biggest = 0;
        for (int i = 0; i < n; i++) {
            if (!g->skin[i] || g->label[i]) continue;
            lbl++;
            int sp = 0, sz = 0; long sx = 0, sy = 0;
            int x0 = gw, x1 = -1, y0 = gh, y1 = -1;
            g->stack[sp++] = i; g->label[i] = lbl;
            while (sp) {
                int c = g->stack[--sp]; sz++;
                int cx = c % gw, cy = c / gw;
                sx += cx; sy += cy;
                x0 = std::min(x0, cx); x1 = std::max(x1, cx); y0 = std::min(y0, cy); y1 = std::max(y1, cy);
                const int nb[4] = {cx > 0 ? c - 1 : -1, cx < gw - 1 ? c + 1 : -1, cy > 0 ? c - gw : -1, cy < gh - 1 ? c + gw : -1};
                for (int q : nb) if (q >= 0 && g->skin[q] && !g->label[q]) { g->label[q] = lbl; g->stack[sp++] = q; }
            }
            biggest = std::max(biggest, sz);
            int bw = x1 - x0 + 1, bh = y1 - y0 + 1;
            float fill = (float)sz / (bw * bh);
            float aspect = (float)bh / bw;
            // face/head: 12+ cells, roughly upright oval, reasonably solid, not most of the frame
            if (sz >= 12 && sz <= n / 4 && aspect >= 0.7f && aspect <= 2.5f && fill >= 0.35f && nb_found < 8)
                blobs[nb_found++] = {sz, (float)sx / sz, (float)sy / sz, bw, bh};
        }
        work_ms += (int)(now_ms() - t0);
        frames++;
        last_blob = biggest;

        // 2b) esp-dl face detector (preferred). Skin blobs are only a fallback.
        bool dl_face = false; float dl_ey = 0;
        if (yuv) {
            if (!det) {
                size_t before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
                det = new HumanFaceDetect();
                ESP_LOGI(TAG, "esp-dl detector created (internal free %u -> %u)", (unsigned)before,
                         (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
            }
            dl::image::img_t img = {copy, (uint16_t)w, (uint16_t)h,
                                    order ? dl::image::DL_IMAGE_PIX_TYPE_UYVY : dl::image::DL_IMAGE_PIX_TYPE_YUYV};
            int64_t td = now_ms();
            auto& res = det->run(img);
            dl_ms += (int)(now_ms() - td); dl_frames++;
            const dl::detect::result_t* bestf = nullptr;
            for (auto& r : res) if (r.score >= 0.5f && (!bestf || r.score > bestf->score)) bestf = &r;
            if (bestf) {
                float fx = (bestf->box[0] + bestf->box[2]) * 0.5f / kCell, fy = (bestf->box[1] + bestf->box[3]) * 0.5f / kCell;
                int bw = std::max(1, (bestf->box[2] - bestf->box[0]) / kCell), bh = std::max(1, (bestf->box[3] - bestf->box[1]) / kCell);
                blobs[0] = {1000, fx - 0.5f, fy - 0.5f, bw, bh};
                nb_found = 1; dl_face = true; dl_hits++;
                dl_ey = (fy - gh * 0.5f) / (gh * 0.5f);
            }
        }
        static int64_t last_dl_t = 0;
        if (dl_face) last_dl_t = now_ms();
        else {
            if (dl_hits > 0) dl_hits--;              // tolerate single DL misses
            // DL saw a face recently: skin blobs are mostly false positives, so hold instead
            if (now_ms() - last_dl_t < 3000) nb_found = 0;
        }

        // 3) Work in head-relative world yaw so the head's own motion doesn't look like the face moving.
        //    target_world = current yaw + angle of the blob from image centre.
        if (head.ManualHold()) { locked = false; cand_hits = 0; continue; }  // user commanded a look
        if (head.Busy()) continue;  // frame taken mid-move: blurred/stale
        int yaw_now = head.Yaw();
        auto world_of = [&](const Blob& b) {
            float ex = ((b.x + 0.5f) - gw * 0.5f) / (gw * 0.5f);
            float off = -ex * kHalfFovX;
            if (invert_x_) off = -off;
            return yaw_now + off;
        };
        int pick = -1; float pick_score = 0;
        for (int k = 0; k < nb_found; k++) {
            float score = (float)blobs[k].sz;
            if (locked) {
                float d = std::fabs(world_of(blobs[k]) - target_world);
                if (d > 20) continue;                 // too far from the tracked face: ignore
                score /= (1.0f + d / 5.0f);           // prefer continuity
            }
            if (score > pick_score) { pick_score = score; pick = k; }
        }
        int64_t tnow = now_ms();
        if (pick >= 0) {
            float w = world_of(blobs[pick]);
            if (!locked) {
                // need 2 consecutive agreeing sightings before locking on
                if (cand_hits > 0 && std::fabs(w - cand_world) < 10) cand_hits++; else cand_hits = 1;
                cand_world = w;
                if (cand_hits >= 2) { locked = true; target_world = w; ESP_LOGI(TAG, "lock at yaw %.0f (blob %d)", w, blobs[pick].sz); }
            } else {
                target_world += 0.4f * (w - target_world);  // EMA smoothing
            }
            last_seen = tnow;
        } else if (locked && tnow - last_seen > 2500) {
            locked = false; cand_hits = 0;
            ESP_LOGI(TAG, "lost face, holding yaw %d", yaw_now);
        }
        if (locked) {
            faces += pick >= 0;
            float err = target_world - yaw_now;
            int ty = yaw_now;
            if (std::fabs(err) > 4.0f) {                      // deadband
                int step = (int)std::lround(err * 0.7f);       // proportional, rate-limited
                step = std::max(-kMaxStep, std::min(kMaxStep, step));
                ty = std::max(-45, std::min(45, yaw_now + step));
            }
            int tp = kLevelPitch;
            if (dl_hits >= 2) {
                // reliable detector: allow gentle pitch (seated face is usually above a desk robot)
                float want = head.Pitch() - dl_ey * kHalfFovY;
                target_pitch += 0.3f * (want - target_pitch);
                target_pitch = std::max(0.0f, std::min(20.0f, target_pitch));
            } else if (!dl_face) {
                target_pitch += 0.2f * (kLevelPitch - target_pitch);  // drift back to level
            }
            tp = (int)std::lround(target_pitch);
            if (std::abs(tp - head.Pitch()) < 3) tp = head.Pitch();
            if (ty != yaw_now || tp != head.Pitch()) head.Track(ty, tp, 40);
            else head.NoteFace();  // hold position, suppress idle glances
            if (tnow - det_log_t >= 1000) {
                ESP_LOGI(TAG, "track %s blobs=%d sz=%d target=%.1f yaw %d->%d pitch %d->%d", dl_face ? "DL" : "skin",
                         nb_found, pick >= 0 ? blobs[pick].sz : 0, target_world, yaw_now, ty, head.Pitch(), tp);
                det_log_t = tnow;
            }
        }
        if (now_ms() - stat_t >= 10000) {
            ESP_LOGI(TAG, "dl_avg=%dms frames=%d faces=%d last_blob=%d avg_work=%dms yaw=%d pitch=%d int_free=%u psram_free=%u",
                     dl_frames ? dl_ms / dl_frames : 0, frames, faces, last_blob, frames ? work_ms / frames : 0, head.Yaw(), head.Pitch(),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
            frames = faces = work_ms = 0; dl_frames = dl_ms = 0;
            stat_t = now_ms();
        }
    }
}

void FaceTracker::RegisterMcpTools() {
    auto& mcp = McpServer::GetInstance();
    mcp.AddTool("robot.face_tracking",
                "Turn face tracking on or off. When on, the robot uses its camera to find a face and turns "
                "its head to follow it (while awake/listening). Setting persists.",
                PropertyList({Property("enabled", kPropertyTypeBoolean)}),
                [this](const PropertyList& props) -> ReturnValue {
                    SetEnabled(props["enabled"].value<bool>());
                    return true;
                });
    mcp.AddTool("robot.face_tracking_invert",
                "Calibration: flip face-tracking direction if the head turns away from the face. "
                "invert_x flips left/right, invert_y flips up/down. Setting persists.",
                PropertyList({Property("invert_x", kPropertyTypeBoolean), Property("invert_y", kPropertyTypeBoolean)}),
                [this](const PropertyList& props) -> ReturnValue {
                    invert_x_ = props["invert_x"].value<bool>();
                    invert_y_ = props["invert_y"].value<bool>();
                    Settings s("face", true);
                    s.SetInt("inv_x", invert_x_ ? 1 : 0);
                    s.SetInt("inv_y", invert_y_ ? 1 : 0);
                    return true;
                });
}
