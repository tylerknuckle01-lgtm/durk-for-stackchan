#include "durk_nfc.h"
#include "durk_sd.h"
#include "application.h"
#include "board.h"
#include "mcp_server.h"
#include "settings.h"
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/idf_additions.h>
#include <atomic>
#include <cstring>
#include <string>

#define TAG "DurkNFC"

namespace durk_nfc {
namespace {
constexpr uint8_t kAddr = 0x50;
// registers
constexpr uint8_t R_OP = 0x02, R_MODE = 0x03, R_BITRATE = 0x04, R_14A = 0x05, R_AUX = 0x0A;
constexpr uint8_t R_MASK0 = 0x16, R_IRQ0 = 0x1A, R_FIFO1 = 0x1E, R_TXB1 = 0x22, R_TXB2 = 0x23, R_ID = 0x3F;
// direct commands
constexpr uint8_t C_SET_DEFAULT = 0xC1, C_STOP = 0xC2, C_TX_CRC = 0xC4, C_TX_NOCRC = 0xC5, C_REQA = 0xC6,
                  C_ADJ_REG = 0xD6, C_CLEAR_FIFO = 0xDB;
constexpr uint8_t OP_EN = 0x80, OP_RX = 0x40, OP_TX = 0x08;
constexpr uint8_t IRQ_OSC = 0x80, IRQ_RXE = 0x10;

i2c_master_dev_handle_t g_dev = nullptr;
std::atomic<bool> g_present{false}, g_enabled{true};
std::string g_last_uid;
std::string g_base;  // read from NVS in Init (internal-RAM stack), never from the PSRAM-stack poll task
int64_t g_last_us = 0;

bool Wr(uint8_t reg, uint8_t v) {
    uint8_t b[2] = {reg, v};
    return i2c_master_transmit(g_dev, b, 2, 50) == ESP_OK;
}
bool Rd(uint8_t reg, uint8_t* out, size_t n = 1) {
    uint8_t a = 0x40 | reg;
    return i2c_master_transmit_receive(g_dev, &a, 1, out, n, 50) == ESP_OK;
}
uint8_t Rd1(uint8_t reg) { uint8_t v = 0; Rd(reg, &v); return v; }
bool Cmd(uint8_t c) { return i2c_master_transmit(g_dev, &c, 1, 50) == ESP_OK; }
void Set(uint8_t reg, uint8_t bits, bool on) {
    uint8_t v = Rd1(reg);
    Wr(reg, on ? (v | bits) : (v & ~bits));
}
uint8_t ReadIrqMain() { uint8_t b[3] = {}; Rd(R_IRQ0, b, 3); return b[0]; }  // reading clears all three

bool WaitIrq(uint8_t mask, int ms) {
    for (int i = 0; i < ms; i++) {
        if (ReadIrqMain() & mask) return true;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return false;
}

int ReadFifo(uint8_t* out, int max) {
    uint8_t s[2] = {};
    Rd(R_FIFO1, s, 2);
    int n = s[0] | ((s[1] & 0xC0) << 2);
    if (n <= 0) return 0;
    if (n > max) n = max;
    uint8_t a = 0x9F;
    if (i2c_master_transmit_receive(g_dev, &a, 1, out, n, 50) != ESP_OK) return 0;
    return n;
}

int Transceive(const uint8_t* tx, int n, bool crc, uint8_t* rx, int max) {
    Cmd(C_CLEAR_FIFO);
    ReadIrqMain();
    Wr(R_TXB2, (uint8_t)((n * 8) & 0xFF));
    Wr(R_TXB1, (uint8_t)((n * 8) >> 8));
    uint8_t buf[16];
    buf[0] = 0x80;  // FIFO load
    memcpy(buf + 1, tx, n);
    if (i2c_master_transmit(g_dev, buf, n + 1, 50) != ESP_OK) return 0;
    Cmd(crc ? C_TX_CRC : C_TX_NOCRC);
    if (!WaitIrq(IRQ_RXE, 8)) return 0;
    return ReadFifo(rx, max);
}

// Returns UID hex ("" if no card).
std::string ReadUid() {
    std::string uid;
    Wr(R_OP, OP_EN | OP_RX | OP_TX);  // field on
    vTaskDelay(pdMS_TO_TICKS(6));     // guard time
    uint8_t aux = Rd1(R_AUX);
    Wr(R_AUX, aux | 0x80);            // no CRC on ATQA / anticollision frames
    Cmd(C_CLEAR_FIFO);
    ReadIrqMain();
    Wr(R_TXB2, 0); Wr(R_TXB1, 0);
    Cmd(C_REQA);
    uint8_t rx[8];
    uint8_t bytes[10];
    int nb = 0;
    if (WaitIrq(IRQ_RXE, 6) && ReadFifo(rx, sizeof(rx)) >= 2) {
        const uint8_t sel[3] = {0x93, 0x95, 0x97};
        for (int lvl = 0; lvl < 3; lvl++) {
            Wr(R_AUX, aux | 0x80);
            Set(R_14A, 0x01, true);  // antcl
            uint8_t ac[2] = {sel[lvl], 0x20};
            int n = Transceive(ac, 2, false, rx, sizeof(rx));
            Set(R_14A, 0x01, false);
            if (n < 5 || (rx[0] ^ rx[1] ^ rx[2] ^ rx[3]) != rx[4]) { nb = 0; break; }
            uint8_t sl[7] = {sel[lvl], 0x70, rx[0], rx[1], rx[2], rx[3], rx[4]};
            Wr(R_AUX, aux & ~0x80);  // SAK has CRC
            uint8_t sak[4];
            if (Transceive(sl, 7, true, sak, sizeof(sak)) < 1) { nb = 0; break; }
            bool cascade = rx[0] == 0x88;
            for (int i = cascade ? 1 : 0; i < 4; i++) bytes[nb++] = rx[i];
            if (!(sak[0] & 0x04)) break;  // UID complete
        }
    }
    Wr(R_AUX, aux);
    Wr(R_OP, OP_EN);  // field off, oscillator stays on
    char h[3];
    for (int i = 0; i < nb; i++) { snprintf(h, sizeof(h), "%02X", bytes[i]); uid += h; }
    return uid;
}

bool ChipInit() {
    uint8_t id = Rd1(R_ID);
    if ((id & 0xF8) != (5 << 3) && (id & 0xF8) != (6 << 3)) {
        ESP_LOGW(TAG, "ST25R3916 not found (id 0x%02X) - NFC off", id);
        return false;
    }
    Cmd(C_SET_DEFAULT);
    vTaskDelay(pdMS_TO_TICKS(2));
    for (uint8_t r = 0; r < 4; r++) Wr(R_MASK0 + r, 0x00);  // report all IRQs in the status regs
    ReadIrqMain();
    Wr(R_OP, OP_EN);
    if (!WaitIrq(IRQ_OSC, 20)) ESP_LOGW(TAG, "oscillator IRQ not seen (continuing)");
    Cmd(C_ADJ_REG);
    vTaskDelay(pdMS_TO_TICKS(6));
    Wr(R_MODE, 0x08);     // ISO14443A initiator
    Wr(R_BITRATE, 0x00);  // 106 kbit/s
    ESP_LOGI(TAG, "ST25R3916 found (id 0x%02X), NFC card reader ready", id);
    return true;
}

std::string ServerBase() {
    std::string url;
    { Settings s("wifi", false); url = s.GetString("ota_url"); }
    if (url.empty()) url = CONFIG_OTA_URL;
    auto pos = url.find("/xiaozhi/ota");
    return pos == std::string::npos ? "" : url.substr(0, pos);
}

void PostUid(const std::string& uid) {
    const std::string& base = g_base;
    auto net = Board::GetInstance().GetNetwork();
    if (base.empty() || !net) return;
    auto http = net->CreateHttp(0);
    http->SetHeader("Content-Type", "application/json");
    http->SetContent("{\"uid\":\"" + uid + "\"}");
    if (http->Open("POST", base + "/durk/nfc")) {
        auto code = http->GetStatusCode();
        ESP_LOGI(TAG, "card %s sent to server (%d)", uid.c_str(), code ? *code : -1);
        http->ReadAll();
    } else {
        ESP_LOGW(TAG, "card %s: server unreachable", uid.c_str());
    }
    http->Close();
}

void PollTask(void*) {
    auto& app = Application::GetInstance();
    vTaskDelay(pdMS_TO_TICKS(8000));  // let boot, Wi-Fi and SD sync settle
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(400));
        if (!g_enabled.load()) continue;
        auto st = app.GetDeviceState();
        if (st != kDeviceStateIdle && st != kDeviceStateListening) continue;  // never during speech/connect/upgrade
        std::string uid = ReadUid();
        if (uid.empty()) continue;
        int64_t now = esp_timer_get_time();
        if (uid == g_last_uid && now - g_last_us < 3000000) { g_last_us = now; continue; }
        g_last_uid = uid;
        g_last_us = now;
        ESP_LOGI(TAG, "card tapped: %s", uid.c_str());
        durk_sd::PlaySfx("chirp");
        PostUid(uid);
    }
}
}  // namespace

void Init(i2c_master_bus_handle_t bus) {
    {
        Settings s("nfc", false);
        g_enabled = s.GetBool("enabled", true);
    }
    g_base = ServerBase();
    if (i2c_master_probe(bus, kAddr, 100) != ESP_OK) {
        ESP_LOGW(TAG, "no device at 0x50 - NFC off");
        return;
    }
    i2c_device_config_t cfg = {};
    cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    cfg.device_address = kAddr;
    cfg.scl_speed_hz = 400000;
    if (i2c_master_bus_add_device(bus, &cfg, &g_dev) != ESP_OK) { g_dev = nullptr; return; }
    if (!ChipInit()) return;
    g_present = true;
    xTaskCreateWithCaps(PollTask, "nfc", 6144, nullptr, 2, nullptr, MALLOC_CAP_SPIRAM);
}

void RegisterMcpTools() {
    auto& mcp = McpServer::GetInstance();
    mcp.AddTool("robot.nfc_enable", "Turn the robot's NFC card reader on or off (saved across reboots).",
                PropertyList({Property("on", kPropertyTypeBoolean)}),
                [](const PropertyList& p) -> ReturnValue {
                    bool on = p["on"].value<bool>();
                    g_enabled = on;
                    Settings s("nfc", true);
                    s.SetBool("enabled", on);
                    return std::string("{\"present\":") + (g_present ? "true" : "false") + ",\"enabled\":" + (on ? "true" : "false") + "}";
                });
}
}  // namespace durk_nfc
