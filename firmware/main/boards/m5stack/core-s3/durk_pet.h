// Durk pet mode: reacts to petting (touch hold/stroke), shake and spin (BMI270 IMU).
// Inspired by Corvelis/stackchan-pet-fw (behaviour only; clean implementation).
#pragma once
#include <driver/i2c_master.h>
#include <string>

namespace durk_pet {
void Init(i2c_master_bus_handle_t bus);   // safe if IMU absent: touch reactions still work
void OnPet();                              // called from touch poll (timer ctx): just flags
void RegisterMcpTools();
bool KidMode();
}
