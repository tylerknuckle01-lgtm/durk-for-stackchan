// Durk: body NFC reader (ST25R3916 @0x50 on the internal I2C bus). Reads ISO14443A card UIDs
// and POSTs them to the server (/durk/nfc). Auto-disables if the chip is absent.
#pragma once
#include <driver/i2c_master.h>

namespace durk_nfc {
void Init(i2c_master_bus_handle_t bus);
void RegisterMcpTools();
}
