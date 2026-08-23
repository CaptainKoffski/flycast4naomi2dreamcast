/*
	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
 */
#include "types.h"
#include "hw/holly/sb.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/holly/holly_intc.h"
#include "hw/sh4/sh4_sched.h"
#include "hw/hwreg.h"

#include "naomi.h"
#include "naomi_cart.h"
#include "naomi_regs.h"
#include "naomi_m3comm.h"
#include "multiboard.h"
#include "serialize.h"
#include "network/output.h"
#include "hw/sh4/modules/modules.h"
#include "oslib/oslib.h"
#include "printer.h"
#include "hw/flashrom/x76f100.h"
#include "midiffb.h"
#include "atomiswave.h"
#include "oslib/i18n.h"
#include "cartlog.h"
#include "hw/sh4/sh4_if.h"   // Phase 3: Sh4cntx (guest pc/sp)
// Phase 2 instrumentation: RAM buffers for watermark scans.
// mem_b/RAM_SIZE = main system RAM (sh4_mem.h, already included); vram/VRAM_SIZE
// (pvr_mem.h); aica::aica_ram/ARAM_SIZE (aica_if.h). Sizes are macros in types.h.
#include "hw/pvr/pvr_mem.h"
#include "hw/pvr/pvr_regs.h"   // Phase 5: TA/FB layout regs for the VRAM profile
#include "hw/aica/aica_if.h"
#include "emulator.h"           // Phase 5 Task 6: dc_savestate/emu.stop/emu.start (TEXERR auto-save)
#include <atomic>
#include <cstdio>              // Phase 5: snprintf for the ARAM histogram line
#include <cstring>             // memcpy (handoff write-truth baselines)

#include <memory>
#include <algorithm>

static std::unique_ptr<NaomiM3Comm> m3comm;
static std::unique_ptr<Multiboard> multiboard;

static X76F100SerialFlash mainSerialId;
static X76F100SerialFlash romSerialId;

static int dmaSchedId = -1;
static int dmaXferDelay = 10;	// cart dma xfer speed, in cycles/byte (default 20 MB/s)

void NaomiBoardIDWrite(const u16 data)
{
	// bit 2: clock
	// bit 3: data
	// bit 4: reset (x76f100 only)
	// bit 5: chip select
	mainSerialId.writeCS(data & 0x20);
	mainSerialId.writeRST(data & 0x10);
	mainSerialId.writeSCL(data & 4);
	mainSerialId.writeSDA(data & 8);
}

u16 NaomiBoardIDRead()
{
	// bit 0 indicates the eeprom is a X76F100, otherwise the BIOS expects an AT93C46
	// bit 3 is xf76f100 SDA
	// bit 4 is at93c46 DO
	return (mainSerialId.readSDA() << 3) | 1;
}

void NaomiGameIDWrite(const u16 data)
{
	romSerialId.writeCS(data & 4);
	romSerialId.writeRST(data & 8);
	romSerialId.writeSCL(data & 2);
	romSerialId.writeSDA(data & 1);
}

u16 NaomiGameIDRead()
{
	return romSerialId.readSDA() << 15;
}

u32 ReadMem_naomi(u32 address, u32 size)
{
	if (unlikely(CurrentCartridge == NULL))
	{
		INFO_LOG(NAOMI, "called without cartridge");
		return 0xFFFF;
	}
	if (!settings.naomi.slave && m3comm != nullptr && address >= NAOMI_COMM_CTRL_addr && address <= NAOMI_COMM_STATUS2_addr)
		return m3comm->ReadMem(address, size);
	if (multiboard != nullptr)
	{
		auto ret = multiboard->readG1(address, size);
		if (ret.second)
			return ret.first;
	}
	return CurrentCartridge->ReadMem(address, size);
}

void WriteMem_naomi(u32 address, u32 data, u32 size)
{
	if (unlikely(CurrentCartridge == NULL))
	{
		INFO_LOG(NAOMI, "called without cartridge");
		return;
	}
	if (address >= NAOMI_COMM_CTRL_addr && address <= NAOMI_COMM_STATUS2_addr)
		cartlog("SERIALPOKE addr=%08x data=%08x\n", address, data);
	if (!settings.naomi.slave && m3comm != nullptr && address >= NAOMI_COMM_CTRL_addr && address <= NAOMI_COMM_STATUS2_addr) {
		m3comm->WriteMem(address, data, size);
		return;
	}
	if (multiboard != nullptr && multiboard->writeG1(address, data, size))
		return;
	CurrentCartridge->WriteMem(address, data, size);
}

static int naomiDmaSched(int tag, int sch_cycl, int jitter, void *arg)
{
	u32 start = SB_GDSTARD;
	u32 len = std::min<int>(((SB_GDLEN + 31) & ~31) - SB_GDLEND, 1024);
	SB_GDLEND += len;
	while (len > 0)
	{
		u32 block_len = len;
		void* ptr = CurrentCartridge->GetDmaPtr(block_len);
		if (block_len == 0)
		{
			INFO_LOG(NAOMI, "Aborted DMA transfer. Read past end of cart?");
			for (u32 i = 0; i < len; i += 8, start += 8)
				addrspace::write64(start, 0);
			break;
		}
		WriteMemBlock_nommu_ptr(start, (u32*)ptr, block_len);
		CurrentCartridge->AdvancePtr(block_len);
		len -= block_len;
		start += block_len;
	}
	SB_GDSTARD = start;
	if (SB_GDLEN <= SB_GDLEND)
	{
		SB_GDST = 0;
		asic_RaiseInterrupt(holly_GDROM_DMA);
		return 0;
	}
	else {
		return std::min<int>(SB_GDLEN - SB_GDLEND, 1024) * dmaXferDelay;
	}
}

// Phase 2 instrumentation: highest non-zero byte in a buffer (backwards scan,
// stops at the water line). ponytail: over-reports if stale non-zero data sits
// high in the region — a conservative upper bound, fine for the cut decision.
static u32 cartlog_high(const u8 *buf, u32 size)
{
	for (u32 i = size; i-- > 0; )
		if (buf[i] != 0)
			return i + 1;
	return 0;
}

static void cartlog_watermarks()
{
	cartlog("WATERMARK region=main used=%x size=%x\n", cartlog_high(&mem_b[0], RAM_SIZE), RAM_SIZE);
	cartlog("WATERMARK region=vram used=%x size=%x\n", cartlog_high(&vram[0], VRAM_SIZE), VRAM_SIZE);
	cartlog("WATERMARK region=aram used=%x size=%x\n", cartlog_high(&aica::aica_ram[0], ARAM_SIZE), ARAM_SIZE);
}

// Phase 5 (sound-RAM fit, naomi-vs-dreamcast §1): ARAM write-truth profile. The
// backwards content scan (WATERMARK) can't tell a real game write from a stale
// or BIOS byte -- it pegged ARAM at the exact 8 MB top => "inconclusive"
// (phase2-measurements.md). Fix: snapshot ARAM once at game handoff (first cart
// DMA) and count bytes that DIFFER from that baseline -- a genuine game/AICA
// sound write. (v1 zeroed ARAM/VRAM instead; that guest-visible mutation broke
// rendering for the whole no-render class -- moeru A/B 2026-08-03. Diff against
// a host-side copy measures the same thing without touching guest state. Blind
// spot: a write of the identical byte value isn't counted -- acceptable.)
// Report the true high-water + changed counts below/above DC's 2 MB, plus a
// 256 KB-bucket histogram so a lone stray write is distinguishable from dense
// usage. Directly answers: does the game's sound data fit DC's 2 MB ARAM?
static u8 *cartlog_aram_base, *cartlog_vram_base, *cartlog_main_base;   // handoff baselines (host-only)
static void cartlog_aram_profile()
{
	const u8 *ram = &aica::aica_ram[0];
	const u8 *base = cartlog_aram_base;
	const u32 size = ARAM_SIZE, BUCK = 0x40000;   // 256 KB buckets
	u32 hist[32] = {0}, nb = size / BUCK;
	if (nb > 32) nb = 32;
	u32 high = 0, nz = 0, nz_below2m = 0;
	u32 chigh = 0, cnz = 0, cnz_below2m = 0;
	for (u32 i = 0; i < size; i += 16) {
		// v4 content counters: the interior of a run of identical 16-byte blocks
		// is never sound content — the GD DIMM firmware sweeps unused ARAM with a
		// repeating 4-byte "DMPD" tag (ikaruga dump 2026-08-04: the entire upper
		// 6 MB was one repeated block; the raw diff counted it all as usage and
		// G3-parked ten families), and silence is runs of zeros. Real audio data
		// essentially never repeats adjacent 16-byte blocks; the first block of
		// each run still counts, so the undercount is one block per run.
		bool dup = i >= 16 && memcmp(ram + i, ram + i - 16, 16) == 0;
		for (u32 j = i; j < i + 16; j++)
			if (ram[j] != (base != nullptr ? base[j] : 0)) {
				nz++; high = j + 1;
				if (j < 0x200000) nz_below2m++;
				u32 b = j / BUCK; if (b < 32) hist[b]++;
				if (!dup) {
					cnz++; chigh = j + 1;
					if (j < 0x200000) cnz_below2m++;
				}
			}
	}
	cartlog("ARAMPROFILE high=%x nz=%x nz_below2m=%x nz_above2m=%x content_high=%x content_below2m=%x content_above2m=%x size=%x\n",
			high, nz, nz_below2m, nz - nz_below2m, chigh, cnz_below2m, cnz - cnz_below2m, size);
	char line[288]; int p = 0;
	for (u32 b = 0; b < nb; b++)
		p += snprintf(line + p, sizeof(line) - p, "%x ", hist[b]);
	cartlog("ARAMHIST %s\n", line);   // nz-byte count per 256 KB bucket (bucket 8+ = past 2 MB)
}

// Phase 5 (VRAM fit): write-truth profile, same diff-vs-baseline method as
// cartlog_aram_profile above (see the v1-zeroing note there). The 9.2 MB
// WATERMARK figure came from the never-cleared content scan -- stale BIOS/boot
// bytes count toward it; diffing against the handoff snapshot excludes them.
// Report true high-water + changed counts below/above DC's 8 MB + a 256
// KB-bucket histogram.
// Blind spot: in Flycast the TA parses display lists into host-side structures
// and rendering happens on the host GPU, so ISP/OL buffers and framebuffers
// never appear as vram-array content (on real HW they occupy VRAM). VRAMREGS
// snapshots their layout registers instead -- the real footprint is
// max(content high-water, TA_*_LIMIT, FB_W/R_SOF extents).
// v8: content_* fields below mask the FB regions out (spec 2026-08-07).
static void cartlog_vram_profile()
{
	const u8 *base = cartlog_vram_base;
	const u32 size = VRAM_SIZE, BUCK = 0x40000;   // 256 KB buckets (64 for Naomi's 16 MB)
	u32 hist[64] = {0}, nb = size / BUCK;
	if (nb > 64) nb = 64;
	// v8 FB masking (spec 2026-08-07-vram-fb-masking-design.md, §6 ruling 2):
	// content_* counters exclude the framebuffer regions the CURRENT video regs
	// point at — FB placement is the arcade build's choice, not fit-relevant
	// content (chocomk parks its flip pair at/above the DC's 8 MB line); a DC
	// port budgets 2 FBs separately (score-side: content + 2*fb_bytes).
	// Sample-time regs only, no sticky union: a stale FB region left by a mode
	// change counts as content again later — truthful-if-rare, documented.
	// FB_W_SOF2 is usually a never-written BIOS default (31 kHz progressive
	// parks the field-2 pointer at 0xc00000); masking it costs nothing when
	// nothing was written there. fb_size: write-side stride (8-byte units)
	// x display height — read/write FBs share dimensions under page flipping
	// (read-side variant: Renderer_if.cpp fb_watch formula).
	const u32 fb_size = (FB_R_SIZE.fb_y_size + 1) * FB_W_LINESTRIDE.stride * 8;
	const u32 fb_sof[3] = { FB_W_SOF1 & VRAM_MASK, FB_W_SOF2 & VRAM_MASK, FB_R_SOF1 & VRAM_MASK };
	u32 high = 0, nz = 0, nz_below8m = 0;
	u32 chigh = 0, cnz = 0, cnz_below8m = 0, fb_masked_nz = 0;
	for (u32 i = 0; i < size; i++)
		if (vram[i] != (base != nullptr ? base[i] : 0)) {
			nz++; high = i + 1;
			if (i < 0x800000) nz_below8m++;
			u32 b = i / BUCK; if (b < 64) hist[b]++;
			// unsigned wrap makes (i - sof < fb_size) a one-compare range check
			bool in_fb = (i - fb_sof[0] < fb_size) || (i - fb_sof[1] < fb_size)
			          || (i - fb_sof[2] < fb_size);
			if (in_fb) {
				fb_masked_nz++;
			} else {
				cnz++; chigh = i + 1;
				if (i < 0x800000) cnz_below8m++;
			}
		}
	cartlog("VRAMPROFILE high=%x nz=%x nz_below8m=%x nz_above8m=%x content_high=%x content_below8m=%x content_above8m=%x fb_bytes=%x fb_masked_nz=%x size=%x\n",
			high, nz, nz_below8m, nz - nz_below8m, chigh, cnz_below8m, cnz - cnz_below8m, fb_size, fb_masked_nz, size);
	char line[576]; int p = 0;
	for (u32 b = 0; b < nb; b++)
		p += snprintf(line + p, sizeof(line) - p, "%x ", hist[b]);
	cartlog("VRAMHIST %s\n", line);   // nz-byte count per 256 KB bucket (bucket 32+ = past 8 MB)
	cartlog("VRAMREGS isp_base=%x isp_limit=%x ol_base=%x ol_limit=%x fb_w_sof1=%x fb_w_sof2=%x fb_r_sof1=%x\n",
			TA_ISP_BASE & VRAM_MASK, TA_ISP_LIMIT & VRAM_MASK,
			TA_OL_BASE & VRAM_MASK, TA_OL_LIMIT & VRAM_MASK,
			FB_W_SOF1 & VRAM_MASK, FB_W_SOF2 & VRAM_MASK, FB_R_SOF1 & VRAM_MASK);
}

// v6 (2026-08-06): main-RAM write-truth. The v1 metric (CARTDMA dest
// high-water) is blind on PIO-loading carts (sgtetris: zero DMA tags despite
// visibly running; gwing2: dma_high_water 0 with 1,344 non-main DMAs — kb
// §4.v) and misses CPU-written data above the last DMA'd asset (spec v1
// limitation). Same diff-vs-handoff-baseline method as cartlog_vram_profile:
// 32 MB Naomi window, counts split at DC's 16 MB cap. Raw diff only — no
// ARAM-style content dedup; no fill artifact is known for main, and kb §8
// discipline adds exclusion signatures only when a control run proves one.
static void cartlog_main_profile()
{
	const u8 *base = cartlog_main_base;
	if (base == nullptr)
		return;   // kb §9: a diff is only as meaningful as its baseline — never emit a vs-zero sample
	const u32 size = RAM_SIZE, BUCK = 0x40000;   // 256 KB buckets (128 for Naomi's 32 MB)
	u32 hist[128] = {0}, nb = size / BUCK;
	if (nb > 128) nb = 128;
	u32 high = 0, nz = 0, nz_below16m = 0;
	for (u32 i = 0; i < size; i++)
		if (mem_b[i] != base[i]) {
			nz++; high = i + 1;
			if (i < 0x1000000) nz_below16m++;
			u32 b = i / BUCK; if (b < 128) hist[b]++;
		}
	cartlog("MAINPROFILE high=%x nz=%x nz_below16m=%x nz_above16m=%x size=%x\n",
			high, nz, nz_below16m, nz - nz_below16m, size);
	char line[1280]; int p = 0;
	for (u32 b = 0; b < nb; b++)
		p += snprintf(line + p, sizeof(line) - p, "%x ", hist[b]);
	cartlog("MAINHIST %s\n", line);   // nz-byte count per 256 KB bucket (bucket 64+ = past 16 MB)
}

// v6: one-shot handoff baseline at the first BULK cart->RAM transfer — first
// cart DMA, or cumulative PIO ROM_DATA reads crossing 32 KB (PIO-loading
// carts fire no DMA at all; BIOS-era header pokes are bytes-to-KB while an
// image load is MBs, so any threshold in that gap separates them — chocomk
// cartlog evidence, 2026-08-06). Host-side SNAPSHOT, never a zero: v1 zeroed
// the guest arrays and broke rendering for the whole no-render class (moeru
// A/B 2026-08-03) — instrumentation must never mutate guest state.
// v4 guard note still applies: a pre-DMA ARM reset (BIOS jingle) may have
// allocated the ARAM baseline via cartlog_aram_rebaseline; the snapshot here
// refreshes it, and each *HANDOFF marker fires exactly once (the harness
// keys handoff detection on these markers).
void cartlog_handoff(const char *trigger)
{
	if (!cartlog_enabled())
		return;
	static bool logged = false;
	if (logged)
		return;
	logged = true;
	if (cartlog_aram_base == nullptr)
		cartlog_aram_base = new u8[ARAM_SIZE];
	memcpy(cartlog_aram_base, &aica::aica_ram[0], ARAM_SIZE);
	cartlog("ARAMHANDOFF baselined size=%x trigger=%s\n", ARAM_SIZE, trigger);
	cartlog_vram_base = new u8[VRAM_SIZE];
	memcpy(cartlog_vram_base, &vram[0], VRAM_SIZE);
	cartlog("VRAMHANDOFF baselined size=%x trigger=%s\n", VRAM_SIZE, trigger);
	cartlog_main_base = new u8[RAM_SIZE];
	memcpy(cartlog_main_base, &mem_b[0], RAM_SIZE);
	cartlog("MAINHANDOFF baselined size=%x trigger=%s\n", RAM_SIZE, trigger);
}

// v6: PIO ROM_DATA read accounting (called from the naomi_cart.cpp funnel).
// Cart reads are MMIO and always route through C code — unlike RAM stores,
// the dynarec fast path cannot bypass this (contrast cartlog_shimwatch).
// Doubles as the PIO-loading handoff trigger and the CARTPIOCNT lower bound.
static unsigned long long cartlog_pio_bytes;
void cartlog_pio_read(unsigned bytes)
{
	if (!cartlog_enabled())
		return;
	cartlog_pio_bytes += bytes;
	if (cartlog_pio_bytes >= (32 << 10))
		cartlog_handoff("pio");
}

// Phase 4 (Task 4, V2) instrumentation: any-write detector for the planned shim
// home, phys 0x0cfc0000-0x0cffffff (== mem_b offset 0x00fc0000-0x00ffffff).
// ponytail: this is a content scan, not a live write-intercept -- the arm64
// dynarec's fast memory path (core/rec-ARM64/rec_arm64.cpp GenWriteMemoryFast /
// GenWriteMemoryImmediate) stores directly into the host-mapped RAM array
// whenever addrspace::virtmemEnabled(), bypassing every C-level write function
// for register-indirect stores (the common case for game code) -- so a hook on
// WriteMem/addrspace::write* would silently miss most writes with dynarec on.
// Scanning actual RAM content (same trick as cartlog_watermarks/cartlog_high
// above) sees the result of a write regardless of which path produced it
// (interpreter, dynarec fast/slow path, or cart DMA memcpy). Sampled at the
// same cadence as the watermark scan; like that scan, a write immediately
// zeroed again before the next sample would be missed -- an accepted,
// pre-existing trade-off in this instrumentation, not a new one.
static void cartlog_shimwatch()
{
	static bool tripped = false;
	if (tripped)
		return;
	const u32 SHIM_LO = 0x00fc0000, SHIM_HI = 0x00ffffff;	// mem_b offset; phys 0x0cfc0000-0x0cffffff
	for (u32 i = SHIM_LO; i <= SHIM_HI; i++)
	{
		if (mem_b[i] != 0)
		{
			cartlog("SHIMWATCH addr=%08x\n", 0x0c000000 + i);
			tripped = true;
			break;
		}
	}
}

// Phase 4 (Task 2, senkosp) instrumentation: write-watch for senkosp's OWN
// planned shim home, mem_b offset 0x00010000-0x00017fff (P1
// 0x8c010000-0x8c018000, 32 KB) -- distinct from cartlog_shimwatch above,
// whose Cleopatra-era window (mem_b 0x00fc0000+) senkosp's relocated heap
// now occupies (docs/kb/relocation-map.md), so it cannot double as senkosp's
// window. Baseline-and-compare, NOT non-zero like cartlog_shimwatch: the
// Naomi BIOS may legitimately write low RAM at boot, and the DC loader
// replaces this window wholesale before the game runs, so only a byte that
// changes after the handoff baseline is game-runtime, not boot noise.
// Reuses cartlog_main_base (Task 6's whole-RAM handoff snapshot,
// cartlog_main_profile's same baseline) rather than keeping a private copy
// -- the baseline is taken at the first cart DMA / 32 KB PIO threshold
// (cartlog_handoff), strictly before this scan can first run (both call
// paths into cartlog_sample() are gated on a non-null handoff baseline), so
// it already satisfies "snapshot at the first sample". Same content-scan
// trade-off as cartlog_shimwatch (dynarec bypasses C-level write functions;
// a write reverted between samples evades the scan -- accepted, not new).
static void cartlog_shimwatch2()
{
	const u8 *base = cartlog_main_base;
	if (base == nullptr)
		return;   // no baseline yet -- same discipline as cartlog_main_profile
	const u32 LO = 0x00010000, HI = 0x00017fff;	// mem_b offset; P1 0x8c010000-0x8c018000
	for (u32 i = LO; i <= HI; i++)
		if (mem_b[i] != base[i])
			cartlog("SHIMWATCH2 addr=%08x was=%02x now=%02x\n", 0x8c000000 + i, base[i], mem_b[i]);
}

// Task 6 state for the deferred TEXERR savestate (see cartlog.h). Plain
// file-scope statics, not function-local, so both the emu-thread setter in
// cartlog_texerr_tick() and the render-thread poller cartlog_texerr_save_poll()
// below share them.
static std::atomic<bool> g_texerrSavePending{false};
static std::atomic<u32> g_texerrSaveCode{0};

// Phase 5 Task 5 extension: texture-error classifier cells (docs/kb/
// phase5-hardware.md senkosp2dreamcast repo, section "Texture-error handler"
// -- classifier table). Failing index 0x8c1a20a0, KAMUI2 error code
// 0x8c1a20a8 (same cell/value collision the T3/T6 write-watch caveat
// documents), live-surface counter 0x8c1a2098. Called from the STARTRENDER
// write path (pvr_regs.cpp) rather than the ~10s profile tick, because
// STARTRENDER fires every vblank -- throttled here to every 64th call
// instead of scanning every frame. Baseline-and-compare, same discipline as
// cartlog_shimwatch2 above: one TEXERR line at the first sample, then only
// when a value changes.
void cartlog_texerr_tick()
{
	if (!cartlog_enabled())
		return;
	static unsigned calls;
	if ((calls++ % 64) != 0)
		return;
	// mem_b offset; P1 0x8c1a20a0 / 0x8c1a20a8 / 0x8c1a2098 (all 4-byte aligned)
	u32 idx  = *(const u32 *)&mem_b[0x001a20a0];
	u32 code = *(const u32 *)&mem_b[0x001a20a8];
	u32 cnt  = *(const u32 *)&mem_b[0x001a2098];
	static bool have_baseline;
	static u32 last_idx, last_code, last_cnt;
	if (!have_baseline || idx != last_idx || code != last_code || cnt != last_cnt)
	{
		cartlog("TEXERR idx=%08x code=%08x d98=%08x\n", idx, code, cnt);
		last_idx = idx; last_code = code; last_cnt = cnt;
		have_baseline = true;
	}

	// Task 6: arm the one-shot savestate on the code cell's 0->nonzero
	// transition. Independent of the print-throttle statics above (this must
	// see every throttled sample, not just the ones that changed) and of its
	// own one-shot latch (armed_once) so a later code==0 (cell reused by a
	// benign call, docs/kb/phase5-hardware.md "no clear-on-read semantics")
	// can never re-arm a second save. This function runs on the emu thread
	// (called from the STARTRENDER write path) -- it only sets the flag;
	// cartlog_texerr_save_poll() (render thread) does the actual save.
	static bool have_prev_code, armed_once;
	static u32 prev_code;
	if (!armed_once && have_prev_code && prev_code == 0 && code != 0)
	{
		armed_once = true;
		g_texerrSaveCode.store(code, std::memory_order_relaxed);
		g_texerrSavePending.store(true, std::memory_order_release);
	}
	prev_code = code;
	have_prev_code = true;
}

void cartlog_texerr_save_poll()
{
	if (!cartlog_enabled())
		return;
	if (!g_texerrSavePending.exchange(false, std::memory_order_acquire))
		return;
	u32 code = g_texerrSaveCode.load(std::memory_order_relaxed);
	if (!dc_savestateAllowed())
	{
		cartlog("TEXERRSAVE FAILED code=%08x reason=not-allowed\n", code);
		return;
	}
	// index 0 == default slot, no "_N" filename suffix (oslib.cpp
	// getSavestatePath) -- matches the Phase 3 canary-snapshot precedent
	// (docs/kb/tooling.md "Phase 3: RAM snapshot"). Query the path with the
	// same (index, writable) args dc_savestate(0) itself uses, so the
	// logged path is exactly where the file lands, not a guess.
	std::string path = hostfs::getSavestatePath(0, true);
	try {
		emu.stop();     // must run from the render thread: joins the emu
		                // thread's own std::async result (checkStatus()) --
		                // calling this from the emu thread itself deadlocks.
		dc_savestate(0);
		emu.start();
		cartlog("TEXERRSAVE code=%08x slot=0 %s\n", code, path.c_str());
	} catch (const FlycastException& e) {
		cartlog("TEXERRSAVE FAILED code=%08x reason=%s\n", code, e.what());
	}
}

static void cartlog_sample()
{
	cartlog_watermarks();
	cartlog_shimwatch();   // Phase 4 (Task 4, V2): shim-home content scan, same cadence
	cartlog_shimwatch2();  // Phase 4 (Task 2, senkosp): senkosp's own shim-home window, same cadence
	cartlog_aram_profile();   // Phase 5: sound-RAM fit (write-truth, post-handoff)
	cartlog_vram_profile();   // Phase 5: VRAM fit (write-truth, post-handoff)
	cartlog_main_profile();   // v6: main-RAM fit (write-truth, post-handoff)
	cartlog_sp_water();       // Phase 4 (Task 1): r15 water-mark across maple transactions
	cartlog("CARTPIOCNT bytes=%llx\n", cartlog_pio_bytes);
	// v4 diagnostics: raw ARAM snapshot, overwritten each sample — ground truth for
	// "is the above-cap diff real sound content or an init-fill sweep" (2026-08-04)
	if (const char *dump = getenv("FLYCAST_ARAMDUMP")) {
		if (FILE *df = fopen(dump, "wb")) {
			fwrite(&aica::aica_ram[0], 1, ARAM_SIZE, df);
			fclose(df);
		}
	}
}

// v4 (2026-08-04): the Naomi BIOS sound-RAM test sweeps all 8 MB with a nonzero
// pattern, and on many titles that sweep lands AFTER the first cart DMA — a
// first-DMA baseline then counts test residue as game sound usage forever
// (ikaruga/ausfache cohort: exactly 0x600000 changed bytes above the DC cap,
// byte-identical across the v2-zeroing and v3-snapshot semantics). Every AICA
// ARM reset assert re-snapshots the baseline: the game's own sound-driver
// upload is the last assert before steady state, so the final baseline lands
// after the BIOS sweep. parse_capture restarts its ARAM running-max at the
// last ARAMREBASE marker.
// ponytail: a title that never uploads its own ARM driver keeps the polluted
// baseline — shows up as that exact-0x600000 signature; none observed yet.
void cartlog_aram_rebaseline()
{
	if (!cartlog_enabled() || !settings.platform.isNaomi())
		return;
	if (cartlog_aram_base == nullptr)
		cartlog_aram_base = new u8[ARAM_SIZE];
	memcpy(cartlog_aram_base, &aica::aica_ram[0], ARAM_SIZE);
	cartlog("ARAMREBASE armrst size=%x\n", ARAM_SIZE);
}

// v4 (2026-08-04): cart-DMA-only sampling misses titles that stop DMAing once
// loaded — ikaruga's steady state was never sampled and its full-window run
// parsed as no-render. One sample every 600 vblanks (~10 s).
void cartlog_profiles_tick()
{
	if (cartlog_aram_base == nullptr || !cartlog_enabled())   // null until first cart DMA => Naomi game only
		return;
	static u32 vblanks = 0;
	if (++vblanks % 600 != 0)
		return;
	cartlog_sample();
}

//Dma Start
static void Naomi_DmaStart(u32 addr, u32 data)
{
	if ((data & 1) == 0 || SB_GDST == 1)
		return;
	if (SB_GDEN == 0)
	{
		INFO_LOG(NAOMI, "Invalid NAOMI-DMA start, SB_GDEN=0. Ignoring it.");
		return;
	}
	
	if (multiboard != nullptr && multiboard->dmaStart())
	{
	}
	else if ((m3comm == nullptr || !m3comm->DmaStart(addr, data)) && CurrentCartridge != nullptr)
	{
		DEBUG_LOG(NAOMI, "NAOMI-DMA start addr %08X len %x", SB_GDSTAR, SB_GDLEN);
		cartlog("CARTDMA src=%08x dest=%08x len=%x\n",
				CurrentCartridge->GetDmaSrcOffset(), SB_GDSTAR & 0x1FFFFFE0, SB_GDLEN);
		cartlog("CARTDMAPC pc=%08x sp=%08x\n", Sh4cntx.pc, Sh4cntx.r[15]);   // Phase 3: guest PC/SP at DMA kick
		cartlog_handoff("dma");   // Phase 5/v6: one-shot 3-region baseline (see cartlog_handoff)
		static u32 cartlog_dma_count = 0;
		if ((cartlog_dma_count++ & 63) == 0)   // ponytail: every 64th DMA; the scan is cheap but not free
			cartlog_sample();
		verify(1 == SB_GDDIR);
		SB_GDST = 1;
		SB_GDSTARD = SB_GDSTAR & 0x1FFFFFE0;
		SB_GDLEND = 0;
		// Max G1 bus rate: 50 MHz x 16 bits
		// SH4_access990312_e.xls: 14.4 MB/s from GD-ROM to system RAM
		// Here: 20 MB/s
		sh4_sched_request(dmaSchedId, std::min<int>(SB_GDLEN, 1024) * dmaXferDelay);
		return;
	}
	else
	{
		SB_GDSTARD = SB_GDSTAR + SB_GDLEN;
		SB_GDLEND = SB_GDLEN;
	}
	asic_RaiseInterrupt(holly_GDROM_DMA);
}

void Naomi_setDmaDelay()
{
	if (settings.platform.isAtomiswave() || settings.content.gameId == "FORCE FIVE"
			|| settings.content.gameId == "KENJU")
		// 7 MB/s for Atomiwave games and conversions
		dmaXferDelay = 27;
	else
		dmaXferDelay = 10;
}

static void Naomi_DmaEnable(u32 addr, u32 data)
{
	SB_GDEN = data & 1;
	if (SB_GDEN == 0 && SB_GDST == 1)
	{
		INFO_LOG(NAOMI, "NAOMI-DMA aborted");
		SB_GDST = 0;
		sh4_sched_request(dmaSchedId, -1);
	}
}

void naomi_reg_Init()
{
	static const u8 romSerialData[0x84] = {
		0x19, 0x00, 0xaa, 0x55,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x69, 0x79, 0x68, 0x6b, 0x74, 0x6d, 0x68, 0x6d,
		0xa1, 0x09, ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',
		' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ', ' ',  ' ', ' ', ' ', ' ',
		'0', '0', '0', '0', '0', '0', '0', '0', '0', '0', '0', '0', '0', '0', '0', '0'
	};
	romSerialId.setData(romSerialData);
	mainSerialId.setData(romSerialData);
	if (dmaSchedId == -1)
		dmaSchedId = sh4_sched_register(0, naomiDmaSched);
}

// Sets the full content of the rom board serial eeprom (132 bytes)
// including response to reset and read/write passwords.
void setGameSerialId(const u8 *data)
{
	romSerialId.setData(data);
}

// Return the protected data from the rom board serial eeprom (112 bytes)
// excluding response to reset and passwords.
const u8 *getGameSerialId()
{
	return romSerialId.getProtectedData();
}

void naomi_reg_Term()
{
	multiboard.reset();
	m3comm.reset();
	networkOutput.term();
	if (dmaSchedId != -1)
		sh4_sched_unregister(dmaSchedId);
	dmaSchedId = -1;
	midiffb::term();
}

void naomi_reg_Reset(bool hard)
{
	hollyRegs.setWriteHandler<SB_GDST_addr>(Naomi_DmaStart);
	hollyRegs.setWriteHandler<SB_GDEN_addr>(Naomi_DmaEnable);
	SB_GDST = 0;
	SB_GDEN = 0;
	sh4_sched_request(dmaSchedId, -1);

	atomiswave::reset();

	if (hard)
	{
		naomi_cart_Close();
		multiboard.reset();
		m3comm.reset();
		if (settings.platform.isNaomi())
			m3comm = std::make_unique<NaomiM3Comm>();
		if (settings.naomi.multiboard)
			multiboard = std::make_unique<Multiboard>();
		networkOutput.reset();
		mainSerialId.reset();
		romSerialId.reset();
	}
	else
	{
		if (multiboard != nullptr)
			multiboard->reset();
		if (m3comm != nullptr)
			m3comm->closeNetwork();
	}
	midiffb::reset();
}

void naomi_Serialize(Serializer& ser)
{
	mainSerialId.serialize(ser);
	romSerialId.serialize(ser);
	atomiswave::serialize(ser);
	// TODO serialize m3comm?
	midiffb::serialize(ser);
	sh4_sched_serialize(ser, dmaSchedId);
}
void naomi_Deserialize(Deserializer& deser)
{
	if (deser.version() < Deserializer::V40)
	{
		deser.skip<u32>();	// GSerialBuffer
		deser.skip<u32>();	// BSerialBuffer
		deser.skip<int>();	// GBufPos
		deser.skip<int>();	// BBufPos
		deser.skip<int>();	// GState
		deser.skip<int>();	// BState
		deser.skip<int>();	// GOldClk
		deser.skip<int>();	// BOldClk
		deser.skip<int>();	// BControl
		deser.skip<int>();	// BCmd
		deser.skip<int>();	// BLastCmd
		deser.skip<int>();	// GControl
		deser.skip<int>();	// GCmd
		deser.skip<int>();	// GLastCmd
		deser.skip<int>();	// SerStep
		deser.skip<int>();	// SerStep2
		deser.skip(69);		// BSerial
		deser.skip(69);		// GSerial
	}
	else
	{
		mainSerialId.deserialize(deser);
		romSerialId.deserialize(deser);
	}
	if (deser.version() < Deserializer::V36)
	{
		deser.skip<u32>(); // reg_dimm_command;
		deser.skip<u32>(); // reg_dimm_offsetl;
		deser.skip<u32>(); // reg_dimm_parameterl;
		deser.skip<u32>(); // reg_dimm_parameterh;
		deser.skip<u32>(); // reg_dimm_status;
	}
	atomiswave::deserialize(deser);
	midiffb::deserialize(deser);
	if (deser.version() >= Deserializer::V45)
		sh4_sched_deserialize(deser, dmaSchedId);
}

struct DriveSimPipe : public SerialPort::Pipe
{
	void write(u8 data) override
	{
		if (buffer.empty() && data != 2)
			return;
		if (buffer.size() == 7)
		{
			u8 checksum = 0;
			for (u8 b : buffer)
				checksum += b;
			if (checksum == data)
			{
				int newTacho = (buffer[2] - 1) * 100;
				if (newTacho != tacho)
				{
					tacho = newTacho;
					networkOutput.output("tachometer", tacho);
				}
				int newSpeed = buffer[3] - 1;
				if (newSpeed != speed)
				{
					speed = newSpeed;
					networkOutput.output("speedometer", speed);
				}
				if (!config::NetworkOutput)
				{
					std::string message = strprintf(i18n::T("Speed: %3d"), speed);
					os_notify(message.c_str(), 1000);
				}
			}
			buffer.clear();
		}
		else
		{
			buffer.push_back(data);
		}
	}

	void reset()
	{
		buffer.clear();
		tacho = -1;
		speed = -1;
	}
private:
	std::vector<u8> buffer;
	int tacho = -1;
	int speed = -1;
};

void initDriveSimSerialPipe()
{
	static DriveSimPipe pipe;

	pipe.reset();
	SCIFSerialPort::Instance().setPipe(&pipe);
}

class G2PrinterConnection
{
public:
	u32 read(u32 addr, u32 size);
	void write(u32 addr, u32 size, u32 data);

	static constexpr u32 STATUS_REG_ADDR = 0x1018000;
	static constexpr u32 DATA_REG_ADDR = 0x1010000;

private:
	u32 printerStat = 0xf;
};

static G2PrinterConnection g2PrinterConnection;

u32 G2PrinterConnection::read(u32 addr, u32 size)
{
	if (addr == STATUS_REG_ADDR)
	{
		u32 ret = printerStat;
		printerStat |= 1;
		DEBUG_LOG(NAOMI, "Printer status == %x", ret);
		return ret;
	}
	else
	{
		INFO_LOG(NAOMI, "Unhandled G2 Ext read<%d> at %x", size, addr);
		return 0;
	}
}

void G2PrinterConnection::write(u32 addr, u32 size, u32 data)
{
	switch (addr)
	{
	case DATA_REG_ADDR:
		for (u32 i = 0; i < size; i++)
			printer::print((char)(data >> (i * 8)));
		break;

	case STATUS_REG_ADDR:
		DEBUG_LOG(NAOMI, "Printer status = %x", data);
		printerStat &= ~1;
		break;

	default:
		INFO_LOG(NAOMI, "Unhandled G2 Ext write<%d> at %x: %x", size, addr, data);
		break;
	}
}

//Area 0 , 0x01000000- 0x01FFFFFF      [G2 Ext. Device]
u32 g2ext_readMem(u32 addr, u32 size)
{
	if (addr == G2PrinterConnection::STATUS_REG_ADDR || addr == G2PrinterConnection::DATA_REG_ADDR)
		return g2PrinterConnection.read(addr, size);
	if (multiboard != nullptr)
		return multiboard->readG2Ext(addr, size);

	DEBUG_LOG(NAOMI, "Unhandled G2 Ext read<%d> at %x", size, addr);
	return 0;
}

void g2ext_writeMem(u32 addr, u32 data, u32 size)
{
	if (addr == G2PrinterConnection::STATUS_REG_ADDR || addr == G2PrinterConnection::DATA_REG_ADDR)
		g2PrinterConnection.write(addr, size, data);
	else if (multiboard != nullptr)
		multiboard->writeG2Ext(addr, data, size);
	else
		DEBUG_LOG(NAOMI, "Unhandled G2 Ext write<%d> at %x: %x", size, addr, data);
}
