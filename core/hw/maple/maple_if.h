#pragma once
#include "maple_devs.h"
#include <memory>

extern std::shared_ptr<maple_device> MapleDevices[MAPLE_PORTS][6];

void maple_Init();
void maple_Reset(bool Manual);
void maple_Term();
void maple_ReconnectDevices();
void maple_ReconnectDevice(int bus, int port);

void maple_vblank();

// Phase 4 (Task 1): which caller reached maple_DoDma() for the in-flight
// transaction -- "reg" (guest SB_MDST store, attributable PC) or "vbl"
// (hardware vblank trigger, no guest store). boot-binary.md "Why three
// checks cannot pass as written".
const char *maple_getTrig();
