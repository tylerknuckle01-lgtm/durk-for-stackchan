#include "durk_sd.h"
#include "application.h"
#include "board.h"
#include "display.h"
#include "mcp_server.h"
#include "settings.h"
#include <dirent.h>
#include <driver/gpio.h>
#include <driver/sdspi_host.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_vfs_fat.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <freertos/idf_additions.h>
#include <sdmmc_cmd.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cJSON.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <map>
#include <mutex>

#define TAG "DurkSD"

namespace durk_sd {
namespace {
constexpr gpio_num_t kMisoDc = GPIO_NUM_35;
constexpr gpio_num_t kCs = GPIO_NUM_4;
esp_lcd_panel_io_handle_t g_io = nullptr;
Display* g_disp = nullptr;
sdmmc_card_t* g_card = nullptr;
bool g_mounted = false;
esp_err_t (*g_orig_tx)(sdspi_dev_handle_t, sdmmc_command_t*) = nullptr;
std::mutex g_sd_mu;  // serialises our own SD users

// --- shared-pin dance: drain LCD, release GPIO35 to the card, then give it back as D/C ---
esp_err_t SharedTx(sdspi_dev_handle_t h, sdmmc_command_t* cmd) {
    DisplayLockGuard lock(g_disp);              // no new LVGL flush can start
    if (g_io) esp_lcd_panel_io_tx_param(g_io, 0x00, nullptr, 0);  // ILI9342 NOP: waits for queued colour DMA
    gpio_set_direction(kMisoDc, GPIO_MODE_INPUT);
    esp_err_t r = g_orig_tx(h, cmd);
    gpio_set_direction(kMisoDc, GPIO_MODE_OUTPUT);
    return r;
}

// --- log mirror ---
constexpr size_t kRing = 64 * 1024;
char* g_ring = nullptr;
size_t g_head = 0, g_flushed = 0;  // monotonically increasing byte counters
SemaphoreHandle_t g_log_mu = nullptr;
vprintf_like_t g_prev_vprintf = nullptr;
char g_line[384];

int LogHook(const char* fmt, va_list ap) {
    va_list ap2;
    va_copy(ap2, ap);
    int r = g_prev_vprintf ? g_prev_vprintf(fmt, ap) : vprintf(fmt, ap);
    if (g_ring && g_log_mu && xSemaphoreTake(g_log_mu, 0) == pdTRUE) {
        int n = vsnprintf(g_line, sizeof(g_line), fmt, ap2);
        if (n > 0) {
            n = std::min<int>(n, sizeof(g_line) - 1);
            for (int i = 0; i < n; i++) g_ring[(g_head + i) % kRing] = g_line[i];
            g_head += n;
            if (g_head - g_flushed > kRing) g_flushed = g_head - kRing;  // overrun: drop oldest
        }
        xSemaphoreGive(g_log_mu);
    }
    va_end(ap2);
    return r;
}

constexpr uint64_t kLogCap = 50ull * 1024 * 1024, kFileCap = 5ull * 1024 * 1024;

void EnforceCap() {
    std::string dir = std::string(kMount) + "/logs";
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    std::vector<std::pair<std::string, uint64_t>> files;
    uint64_t total = 0;
    while (auto* e = readdir(d)) {
        if (strncmp(e->d_name, "durk-", 5) != 0) continue;
        struct stat st;
        std::string p = dir + "/" + e->d_name;
        if (stat(p.c_str(), &st) == 0) { files.push_back({p, (uint64_t)st.st_size}); total += st.st_size; }
    }
    closedir(d);
    std::sort(files.begin(), files.end());
    for (auto& f : files) {
        if (total <= kLogCap) break;
        remove(f.first.c_str());
        total -= f.second;
    }
}

std::string LogPath() {
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    char name[48];
    if (t.tm_year + 1900 < 2025) snprintf(name, sizeof(name), "durk-nodate.log");
    else snprintf(name, sizeof(name), "durk-%04d%02d%02d.log", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
    std::string base = std::string(kMount) + "/logs/" + name, p = base;
    struct stat st;
    for (int k = 1; k < 50 && stat(p.c_str(), &st) == 0 && (uint64_t)st.st_size > kFileCap; k++)
        p = base.substr(0, base.size() - 4) + "-" + std::to_string(k) + ".log";
    return p;
}

void FlushTask(void*) {
    std::string chunk;
    int since_cap = 0;
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(3000));
        // wait for SNTP (or ~60 s) so the first file gets a real date
        static int waited = 0;
        if (time(nullptr) < 1735689600 && waited++ < 20) continue;
        chunk.clear();
        xSemaphoreTake(g_log_mu, portMAX_DELAY);
        size_t n = g_head - g_flushed;
        if (n > 16384) n = 16384;
        chunk.resize(n);
        for (size_t i = 0; i < n; i++) chunk[i] = g_ring[(g_flushed + i) % kRing];
        g_flushed += n;
        xSemaphoreGive(g_log_mu);
        if (chunk.empty()) continue;
        std::lock_guard<std::mutex> g(g_sd_mu);
        static std::string last_path;
        std::string path = LogPath();
        if (path != last_path) { last_path = path; ESP_LOGI(TAG, "logging to %s", path.c_str()); }
        FILE* f = fopen(path.c_str(), "a");
        if (f) { fwrite(chunk.data(), 1, chunk.size(), f); fclose(f); }
        if (++since_cap >= 100) { since_cap = 0; EnforceCap(); }
    }
}

const char* ResetName(esp_reset_reason_t r) {
    switch (r) {
        case ESP_RST_POWERON: return "POWERON"; case ESP_RST_SW: return "SOFTWARE";
        case ESP_RST_PANIC: return "PANIC (crash)"; case ESP_RST_INT_WDT: return "INT_WDT";
        case ESP_RST_TASK_WDT: return "TASK_WDT"; case ESP_RST_WDT: return "WDT";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP"; case ESP_RST_BROWNOUT: return "BROWNOUT";
        case ESP_RST_EXT: return "EXTERNAL"; case ESP_RST_USB: return "USB";
        default: return "OTHER";
    }
}

// --- sound effects ---
std::map<std::string, std::string> g_sfx;  // name -> ogg bytes
std::mutex g_sfx_mu;
QueueHandle_t g_sfx_q = nullptr;

void LoadSfxLocked() {
    std::string dir = std::string(kMount) + "/sfx";
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    while (auto* e = readdir(d)) {
        std::string n = e->d_name;
        if (n.size() < 5 || strcasecmp(n.c_str() + n.size() - 4, ".ogg") != 0) continue;
        FILE* f = fopen((dir + "/" + n).c_str(), "rb");
        if (!f) continue;
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz > 0 && sz < 256 * 1024) {
            std::string data(sz, '\0');
            if (fread(data.data(), 1, sz, f) == (size_t)sz) {
                std::lock_guard<std::mutex> g(g_sfx_mu);
                g_sfx[n.substr(0, n.size() - 4)] = std::move(data);
            }
        }
        fclose(f);
    }
    closedir(d);
}

void SfxTask(void*) {
    char name[32];
    while (xQueueReceive(g_sfx_q, name, portMAX_DELAY) == pdTRUE) {
        std::string data;
        {
            std::lock_guard<std::mutex> g(g_sfx_mu);
            auto it = g_sfx.find(name);
            if (it == g_sfx.end()) continue;
            data = it->second;
        }
        ESP_LOGI(TAG, "sfx %s (%u B)", name, (unsigned)data.size());
        Application::GetInstance().PlaySound(data);
    }
}

std::string ServerBase() {
    std::string url;
    { Settings s("wifi", false); url = s.GetString("ota_url"); }
    if (url.empty()) url = CONFIG_OTA_URL;
    auto pos = url.find("/xiaozhi/ota");
    return pos == std::string::npos ? "" : url.substr(0, pos);
}

bool HttpGet(const std::string& url, std::string& out) {
    auto net = Board::GetInstance().GetNetwork();
    if (!net) return false;
    auto http = net->CreateHttp(0);
    if (!http->Open("GET", url)) return false;
    auto code = http->GetStatusCode();
    bool ok = code && *code == 200;
    if (ok) out = http->ReadAll();
    http->Close();
    return ok;
}

// Fetch /durk/sfx and /durk/faces from the server if missing on the card, then play the boot sound.
void SyncTask(void*) {
    auto& app = Application::GetInstance();
    for (int i = 0; i < 120 && app.GetDeviceState() != kDeviceStateIdle; i++) vTaskDelay(pdMS_TO_TICKS(1000));
    vTaskDelay(pdMS_TO_TICKS(1500));
    std::string base = ServerBase();
    int fetched = 0;
    for (const char* kind : {"sfx", "faces"}) {
        std::string idx;
        if (base.empty() || !HttpGet(base + "/durk/" + kind + "/index.txt", idx)) {
            ESP_LOGW(TAG, "no %s index from server", kind);
            continue;
        }
        size_t p = 0;
        while (p < idx.size()) {
            size_t e = idx.find('\n', p);
            std::string fn = idx.substr(p, e == std::string::npos ? std::string::npos : e - p);
            p = e == std::string::npos ? idx.size() : e + 1;
            while (!fn.empty() && (fn.back() == '\r' || fn.back() == ' ')) fn.pop_back();
            if (fn.empty() || fn.find('/') != std::string::npos || fn.find("..") != std::string::npos) continue;
            std::string path = std::string(kMount) + "/" + kind + "/" + fn;
            struct stat st;
            { std::lock_guard<std::mutex> g(g_sd_mu); if (stat(path.c_str(), &st) == 0) continue; }
            std::string data;
            if (!HttpGet(base + "/durk/" + kind + "/" + fn, data) || data.empty()) continue;
            std::lock_guard<std::mutex> g(g_sd_mu);
            FILE* f = fopen(path.c_str(), "wb");
            if (f) { fwrite(data.data(), 1, data.size(), f); fclose(f); fetched++; }
        }
    }
    if (fetched) {
        ESP_LOGI(TAG, "downloaded %d files from server", fetched);
        std::lock_guard<std::mutex> g(g_sd_mu);
        LoadSfxLocked();
    }
    PlaySfx("boot");
    vTaskSuspend(nullptr);  // PSRAM-stack task: park instead of self-delete
}
}  // namespace

bool Mounted() { return g_mounted; }

bool Init(esp_lcd_panel_io_handle_t panel_io, Display* display) {
    g_io = panel_io;
    g_disp = display;
    // log ring + hook first, so boot logs are captured even if the card is absent
    g_ring = (char*)heap_caps_malloc(kRing, MALLOC_CAP_SPIRAM);
    g_log_mu = xSemaphoreCreateMutex();
    if (g_ring) g_prev_vprintf = esp_log_set_vprintf(LogHook);

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI3_HOST;
    host.max_freq_khz = 20000;
    g_orig_tx = host.do_transaction;
    host.do_transaction = SharedTx;
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.gpio_cs = kCs;
    slot.host_id = SPI3_HOST;
    esp_vfs_fat_sdmmc_mount_config_t mc = {};
    mc.format_if_mount_failed = false;
    mc.max_files = 4;
    mc.allocation_unit_size = 16 * 1024;
    esp_err_t err = esp_vfs_fat_sdspi_mount(kMount, &host, &slot, &mc, &g_card);
    gpio_set_direction(kMisoDc, GPIO_MODE_OUTPUT);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD mount failed (%s) - continuing without card", esp_err_to_name(err));
        return false;
    }
    g_mounted = true;
    ESP_LOGI(TAG, "SD mounted: %s, %llu MB", g_card->cid.name,
             (unsigned long long)g_card->csd.capacity * g_card->csd.sector_size / (1024 * 1024));
    for (const char* d : {"/sdcard/logs", "/sdcard/sfx", "/sdcard/faces"}) mkdir(d, 0775);
    {
        auto rr = esp_reset_reason();
        FILE* f = fopen("/sdcard/logs/resets.log", "a");
        if (f) { fprintf(f, "boot uptime0 reset_reason=%s (%d)\n", ResetName(rr), (int)rr); fclose(f); }
        ESP_LOGW(TAG, "BOOT reset_reason=%s", ResetName(rr));
    }
    LoadSfxLocked();
    ESP_LOGI(TAG, "%u sound effects loaded", (unsigned)g_sfx.size());
    g_sfx_q = xQueueCreate(4, 32);
    xTaskCreateWithCaps(SfxTask, "sfx", 4096, nullptr, 3, nullptr, MALLOC_CAP_SPIRAM);
    if (g_ring) xTaskCreateWithCaps(FlushTask, "sdlog", 4096, nullptr, 1, nullptr, MALLOC_CAP_SPIRAM);
    xTaskCreateWithCaps(SyncTask, "sdsync", 6144, nullptr, 1, nullptr, MALLOC_CAP_SPIRAM);
    return true;
}

void PlaySfx(const char* name) {
    if (!g_sfx_q || !name) return;
    char buf[32] = {};
    strncpy(buf, name, sizeof(buf) - 1);
    xQueueSend(g_sfx_q, buf, 0);
}

bool HasSfx(const std::string& n) {
    std::lock_guard<std::mutex> g(g_sfx_mu);
    return g_sfx.count(n) > 0;
}

std::vector<std::string> ListSfx() {
    std::lock_guard<std::mutex> g(g_sfx_mu);
    std::vector<std::string> v;
    for (auto& kv : g_sfx) v.push_back(kv.first);
    return v;
}

std::string ReadText(const std::string& path) {
    if (!g_mounted) return "";
    std::lock_guard<std::mutex> g(g_sd_mu);
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return "";
    std::string s;
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0 && s.size() < 16384) s.append(buf, n);
    fclose(f);
    return s;
}

uint8_t* ReadExactPsram(const std::string& path, size_t size) {
    if (!g_mounted || size == 0) return nullptr;
    std::lock_guard<std::mutex> g(g_sd_mu);
    struct stat st;
    if (stat(path.c_str(), &st) != 0 || (size_t)st.st_size != size) return nullptr;
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return nullptr;
    uint8_t* buf = (uint8_t*)heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    if (buf && fread(buf, 1, size, f) != size) { heap_caps_free(buf); buf = nullptr; }
    fclose(f);
    return buf;
}

std::string RecentLogs(size_t max_bytes) {
    if (!g_ring) return "";
    xSemaphoreTake(g_log_mu, portMAX_DELAY);
    size_t n = std::min(max_bytes, std::min(g_head, kRing));
    std::string s(n, '\0');
    for (size_t i = 0; i < n; i++) s[i] = g_ring[(g_head - n + i) % kRing];
    xSemaphoreGive(g_log_mu);
    // strip ANSI colour codes
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\x1b') { while (i < s.size() && s[i] != 'm') i++; continue; }
        out += s[i];
    }
    return out;
}

namespace {
// --- Durk personality memory: one small JSON file, written atomically (temp file + rename) ---
constexpr const char* kMemDir = "/sdcard/memory";
constexpr const char* kMemFile = "/sdcard/memory/durk_memory.json";
constexpr const char* kMemTmp = "/sdcard/memory/durk_memory.tmp";
constexpr size_t kMemCap = 16 * 1024;

cJSON* MemResult(bool ok, const char* err = nullptr) {
    cJSON* r = cJSON_CreateObject();
    cJSON_AddBoolToObject(r, "ok", ok);
    cJSON_AddBoolToObject(r, "sd", g_mounted);
    if (err) cJSON_AddStringToObject(r, "error", err);
    return r;
}

cJSON* MemRead() {
    if (!g_mounted) return MemResult(false, "no sd card");
    std::lock_guard<std::mutex> g(g_sd_mu);
    FILE* f = fopen(kMemFile, "rb");
    if (!f) f = fopen(kMemTmp, "rb");  // power cut between remove and rename
    if (!f) { cJSON* r = MemResult(true); cJSON_AddBoolToObject(r, "exists", false); cJSON_AddStringToObject(r, "data", ""); return r; }
    char* buf = (char*)heap_caps_malloc(kMemCap + 1, MALLOC_CAP_SPIRAM);
    if (!buf) { fclose(f); return MemResult(false, "no memory"); }
    size_t n = fread(buf, 1, kMemCap, f);
    fclose(f);
    buf[n] = 0;
    cJSON* r = MemResult(true);
    cJSON_AddBoolToObject(r, "exists", true);
    cJSON_AddStringToObject(r, "data", buf);
    heap_caps_free(buf);
    return r;
}

cJSON* MemWrite(const std::string& data) {
    if (!g_mounted) return MemResult(false, "no sd card");
    if (data.size() > kMemCap) return MemResult(false, "too large (16 KB max)");
    std::lock_guard<std::mutex> g(g_sd_mu);
    mkdir(kMemDir, 0775);
    FILE* f = fopen(kMemTmp, "wb");
    if (!f) return MemResult(false, "open failed");
    size_t w = fwrite(data.data(), 1, data.size(), f);
    bool ok = (w == data.size()) && fflush(f) == 0 && fsync(fileno(f)) == 0;
    fclose(f);
    if (!ok) { remove(kMemTmp); return MemResult(false, "write failed"); }
    remove(kMemFile);  // FAT rename() does not overwrite
    if (rename(kMemTmp, kMemFile) != 0) return MemResult(false, "rename failed");
    cJSON* r = MemResult(true);
    cJSON_AddNumberToObject(r, "bytes", (double)data.size());
    return r;
}

cJSON* MemInfo() {
    cJSON* r = MemResult(g_mounted, g_mounted ? nullptr : "no sd card");
    if (!g_mounted) return r;
    std::lock_guard<std::mutex> g(g_sd_mu);
    uint64_t total = 0, fr = 0;
    if (esp_vfs_fat_info(kMount, &total, &fr) == ESP_OK) {
        cJSON_AddNumberToObject(r, "total_mb", (double)total / 1048576.0);
        cJSON_AddNumberToObject(r, "free_mb", (double)fr / 1048576.0);
    }
    struct stat st;
    cJSON_AddNumberToObject(r, "memory_bytes", stat(kMemFile, &st) == 0 ? (double)st.st_size : -1);
    cJSON_AddNumberToObject(r, "cap_bytes", (double)kMemCap);
    return r;
}
}  // namespace

void RegisterMcpTools() {
    auto& mcp = McpServer::GetInstance();
    mcp.AddTool("robot.play_sound",
                "Play a short sound effect from the robot's SD card (e.g. boot, wake, timer, laugh, boop, chirp). "
                "Use robot.list_sounds to see what's available.",
                PropertyList({Property("name", kPropertyTypeString)}),
                [](const PropertyList& p) -> ReturnValue {
                    auto n = p["name"].value<std::string>();
                    if (n.size() > 4 && n.substr(n.size() - 4) == ".ogg") n = n.substr(0, n.size() - 4);
                    if (!HasSfx(n)) return false;
                    PlaySfx(n.c_str());
                    return true;
                });
    mcp.AddTool("robot.list_sounds", "List the sound effects available on the robot's SD card.", PropertyList(),
                [](const PropertyList&) -> ReturnValue {
                    std::string s = "{\"sd\":" + std::string(g_mounted ? "true" : "false") + ",\"sounds\":[";
                    bool first = true;
                    for (auto& n : ListSfx()) { s += (first ? "\"" : ",\"") + n + "\""; first = false; }
                    return s + "]}";
                });
    mcp.AddTool("robot.get_logs", "Return the robot's most recent firmware log lines (for debugging).",
                PropertyList({Property("max_chars", kPropertyTypeInteger, 3000, 200, 8000)}),
                [](const PropertyList& p) -> ReturnValue { return RecentLogs(p["max_chars"].value<int>()); });
    // Durk memory sync (server-only; hidden from the LLM by the server's tool filter)
    mcp.AddTool("robot.memory_read", "Internal: read Durk's memory file from the SD card.", PropertyList(),
                [](const PropertyList&) -> ReturnValue { return MemRead(); });
    mcp.AddTool("robot.memory_write", "Internal: replace Durk's memory file on the SD card (atomic, 16 KB max).",
                PropertyList({Property("data", kPropertyTypeString)}),
                [](const PropertyList& p) -> ReturnValue { return MemWrite(p["data"].value<std::string>()); });
    mcp.AddTool("robot.memory_info", "Internal: SD card free space and memory file size.", PropertyList(),
                [](const PropertyList&) -> ReturnValue { return MemInfo(); });
}
}  // namespace durk_sd
