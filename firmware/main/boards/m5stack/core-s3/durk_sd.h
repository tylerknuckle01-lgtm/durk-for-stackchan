// Durk: microSD (FATFS over SPI3, shared with the LCD), sound effects, persistent logs.
#pragma once
#include <esp_lcd_panel_io.h>
#include <string>
#include <vector>

namespace durk_sd {
constexpr const char* kMount = "/sdcard";
// Mount the card. panel_io is used to drain LCD transfers before SD traffic (GPIO35 is LCD D/C and SD MISO).
}  // namespace durk_sd
class Display;
namespace durk_sd {
bool Init(esp_lcd_panel_io_handle_t panel_io, Display* display);
bool Mounted();
// Sound effects (/sdcard/sfx/<name>.ogg, cached in PSRAM). Non-blocking; queued to a worker task.
void PlaySfx(const char* name);
std::vector<std::string> ListSfx();
bool HasSfx(const std::string& name);
// Read a small text file from the card (empty on failure). Runs on the caller's thread: never call with
// the display lock held.
std::string ReadText(const std::string& path);
// Read a binary file of exactly `size` bytes into a new PSRAM buffer (nullptr on any failure).
// Caller frees with heap_caps_free. Never call with the display lock held.
uint8_t* ReadExactPsram(const std::string& path, size_t size);
// Last ~n bytes of the in-RAM log ring.
std::string RecentLogs(size_t max_bytes);
void RegisterMcpTools();
}  // namespace durk_sd
