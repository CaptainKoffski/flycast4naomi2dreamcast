// core/hw/naomi/cartlog.h
// Phase 2 instrumentation (Cleopatra Naomi->DC port). Not upstream.
#pragma once
void cartlog(const char *fmt, ...);
bool cartlog_enabled();
// naomi.cpp — v4 measurement fixes (2026-08-04):
void cartlog_aram_rebaseline();   // re-snapshot ARAM baseline; call on AICA ARM reset assert
void cartlog_profiles_tick();     // periodic ARAM/VRAM profile sample; call once per vblank
// naomi.cpp — v6 main-RAM write-truth (2026-08-06):
void cartlog_handoff(const char *trigger);   // one-shot ARAM/VRAM/MAIN baseline; trigger = "dma" | "pio"
void cartlog_pio_read(unsigned bytes);       // ROM_DATA PIO accounting; fires cartlog_handoff("pio") at 32 KB
