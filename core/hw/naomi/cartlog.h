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
// round 13 — transfer-queue slot-write history (2026-08-16):
void cartlog_ringnote(unsigned pa, unsigned val, unsigned pc, unsigned pr, int sz);
void cartlog_ringdump(const char *why);      // print buffered slot writes, oldest first
// Phase 4 (Task 1): r15 high/low water-mark across maple transactions. A
// single fixed call site (CARTDMAPC) samples SP at one constant call depth
// every time -- that measures nothing about a stack's actual range. Maple
// transactions (MDODMA/MAPLEPC) fire at many different depths within the
// task's frame, so sampling r15 there gives a real water-mark instead.
// boot-binary.md "SP -- two stacks, not one".
void cartlog_sp_sample(unsigned sp);   // call at every maple transaction with Sh4cntx.r[15]
void cartlog_sp_water();               // emit SPWATER; call at the existing ~10s profile tick
