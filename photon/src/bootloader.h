#pragma once

void reboot_into_bootloader();

// ---------------------------------------------------------------------------
// Bus loader (PBL) entry.  PBL is an optional 1 KB loader in the last flash
// page (0x08007C00) that updates this firmware over the RS-485 bus (Marlin
// M485), so feeders can stay mounted.  See VENDOR_OPTIONS sub-commands
// 0x20 / 0x21 in PhotonFeederProtocol.cpp.  Without PBL installed nothing
// changes: bus_loader_version() returns 0 and 0x21 is refused.
// ---------------------------------------------------------------------------
#include <stdint.h>

#define PBL_VENDOR_INFO   0x20   // reply options: 'P','B','L', app protocol, PBL version (0 = none)
#define PBL_VENDOR_ENTER  0x21   // options[1..4] = "PBL!"; reply as INFO, then jump to PBL
#define PBL_ENTER_MAGIC   "PBL!"
#define PBL_APP_PROTOCOL  1

uint8_t bus_loader_version();
void bus_loader_enter(uint8_t address);   // does not return
