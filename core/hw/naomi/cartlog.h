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
// Phase 5 Task 5 extension: texture-error classifier-cell sampler (senkosp).
void cartlog_texerr_tick();            // call on STARTRENDER write; throttles itself to every 64th call
// Phase 5 Task 6: one-shot RAM snapshot on the TEXERR code=0->nonzero
// transition. cartlog_texerr_tick() (emu thread) only arms a flag -- it
// cannot safely call dc_savestate()/emu.stop() itself (those join the emu
// thread's own std::async result; self-join would deadlock). This poll
// function does the actual save and must be called once per rendered frame
// from the UI/render thread (mainui_rend_frame(), NOT the emu thread).
void cartlog_texerr_save_poll();
// Phase 5 fix-scoping (senkosp): KAMUI2 VRAM texture-arena high-water walker.
void cartlog_arena_tick();             // call on STARTRENDER write; prints only on a new running max
// Phase 4 (Task 2, senkosp) / Phase 7 T1 fix round 2: shim-home + isoldr-slot
// write-watch. Was file-local to naomi.cpp; exported so the DC-reachable
// STARTRENDER tick (pvr_regs.cpp) can drive it directly -- the Naomi-only
// ~10s profile tick never fires on a native DC boot (see cartlog_dc_armed).
void cartlog_shimwatch2();
bool cartlog_dc_armed();               // true once the handoff baseline exists (Naomi or DC-CCR trigger)
