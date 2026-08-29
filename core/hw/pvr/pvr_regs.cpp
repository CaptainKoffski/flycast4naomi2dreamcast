#include "pvr_regs.h"
#include "pvr_mem.h"
#include "hw/sh4/sh4_if.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/naomi/cartlog.h"
#include "Renderer_if.h"
#include "ta.h"
#include "spg.h"
#include <map>

bool pal_needs_update=true;

u8 pvr_regs[pvr_RegSize];

#define PVR_REG_NAME(r) { r##_addr, #r },
const std::map<u32, const char *> pvr_reg_names = {
		PVR_REG_NAME(ID)
		PVR_REG_NAME(REVISION)
		PVR_REG_NAME(SOFTRESET)
		PVR_REG_NAME(STARTRENDER)
		PVR_REG_NAME(TEST_SELECT)
		PVR_REG_NAME(PARAM_BASE)
		PVR_REG_NAME(REGION_BASE)
		PVR_REG_NAME(SPAN_SORT_CFG)
		PVR_REG_NAME(VO_BORDER_COL)
		PVR_REG_NAME(FB_R_CTRL)
		PVR_REG_NAME(FB_W_CTRL)
		PVR_REG_NAME(FB_W_LINESTRIDE)
		PVR_REG_NAME(FB_R_SOF1)
		PVR_REG_NAME(FB_R_SOF2)
		PVR_REG_NAME(FB_R_SIZE)
		PVR_REG_NAME(FB_W_SOF1)
		PVR_REG_NAME(FB_W_SOF2)
		PVR_REG_NAME(FB_X_CLIP)
		PVR_REG_NAME(FB_Y_CLIP)
		PVR_REG_NAME(FPU_SHAD_SCALE)
		PVR_REG_NAME(FPU_CULL_VAL)
		PVR_REG_NAME(FPU_PARAM_CFG)
		PVR_REG_NAME(HALF_OFFSET)
		PVR_REG_NAME(FPU_PERP_VAL)
		PVR_REG_NAME(ISP_BACKGND_D)
		PVR_REG_NAME(ISP_BACKGND_T)
		PVR_REG_NAME(ISP_FEED_CFG)
		PVR_REG_NAME(SDRAM_REFRESH)
		PVR_REG_NAME(SDRAM_ARB_CFG)
		PVR_REG_NAME(SDRAM_CFG)
		PVR_REG_NAME(FOG_COL_RAM)
		PVR_REG_NAME(FOG_COL_VERT)
		PVR_REG_NAME(FOG_DENSITY)
		PVR_REG_NAME(FOG_CLAMP_MAX)
		PVR_REG_NAME(FOG_CLAMP_MIN)
		PVR_REG_NAME(SPG_TRIGGER_POS)
		PVR_REG_NAME(SPG_HBLANK_INT)
		PVR_REG_NAME(SPG_VBLANK_INT)
		PVR_REG_NAME(SPG_CONTROL)
		PVR_REG_NAME(SPG_HBLANK)
		PVR_REG_NAME(SPG_LOAD)
		PVR_REG_NAME(SPG_VBLANK)
		PVR_REG_NAME(SPG_WIDTH)
		PVR_REG_NAME(TEXT_CONTROL)
		PVR_REG_NAME(VO_CONTROL)
		PVR_REG_NAME(VO_STARTX)
		PVR_REG_NAME(VO_STARTY)
		PVR_REG_NAME(SCALER_CTL)
		PVR_REG_NAME(PAL_RAM_CTRL)
		PVR_REG_NAME(SPG_STATUS)
		PVR_REG_NAME(FB_BURSTCTRL)
		PVR_REG_NAME(FB_C_SOF)
		PVR_REG_NAME(Y_COEFF)
		PVR_REG_NAME(PT_ALPHA_REF)
		PVR_REG_NAME(TA_OL_BASE)
		PVR_REG_NAME(TA_ISP_BASE)
		PVR_REG_NAME(TA_OL_LIMIT)
		PVR_REG_NAME(TA_ISP_LIMIT)
		PVR_REG_NAME(TA_NEXT_OPB)
		PVR_REG_NAME(TA_ITP_CURRENT)
		PVR_REG_NAME(TA_GLOB_TILE_CLIP)
		PVR_REG_NAME(TA_ALLOC_CTRL)
		PVR_REG_NAME(TA_LIST_INIT)
		PVR_REG_NAME(TA_YUV_TEX_BASE)
		PVR_REG_NAME(TA_YUV_TEX_CTRL)
		PVR_REG_NAME(TA_YUV_TEX_CNT)
		PVR_REG_NAME(TA_LIST_CONT)
		PVR_REG_NAME(TA_NEXT_OPB_INIT)
		PVR_REG_NAME(SIGNATURE1)
		PVR_REG_NAME(SIGNATURE2)
};
#undef PVR_REG_NAME

static const char *regName(u32 paddr)
{
	u32 addr = paddr & pvr_RegMask;
	static char regName[32];
	auto it = pvr_reg_names.find(addr);
	if (it == pvr_reg_names.end())
	{
		if (addr >= FOG_TABLE_START_addr && addr <= FOG_TABLE_END_addr)
			snprintf(regName, sizeof(regName), "FOG_TABLE[%x]", addr - FOG_TABLE_START_addr);
		else if (addr >= TA_OL_POINTERS_START_addr && addr <= TA_OL_POINTERS_END_addr)
			snprintf(regName, sizeof(regName), "TA_OL_POINTERS[%x]", addr - TA_OL_POINTERS_START_addr);
		else if (addr >= PALETTE_RAM_START_addr && addr <= PALETTE_RAM_END_addr)
			snprintf(regName, sizeof(regName), "PALETTE[%x]", addr - PALETTE_RAM_START_addr);
		else
			snprintf(regName, sizeof(regName), "?%08x", paddr);
		return regName;
	}
	else
		return it->second;
}

u32 pvr_ReadReg(u32 addr)
{
	if ((addr & pvr_RegMask) != SPG_STATUS_addr)
		DEBUG_LOG(PVR, "read %s.%c == %x", regName(addr),
				((addr >> 26) & 7) == 2 ? 'b' : (addr & 0x2000000) ? '1' : '0',
						PvrReg(addr, u32));
	return PvrReg(addr,u32);
}

void pvr_WriteReg(u32 paddr,u32 data)
{
	u32 addr = paddr & pvr_RegMask;
	DEBUG_LOG(PVR, "write %s.%c = %x", regName(paddr),
			((paddr >> 26) & 7) == 2 ? 'b' : (paddr & 0x2000000) ? '1' : '0',
					data);

	// Phase 5 round-6 (senkosp G-carve): TA_ISP_LIMIT writer hunt. The shim's
	// post-init dev-word stomp was overwritten (r8a-carve leg: registers show
	// the library carve, stomp values gone by render 1500) -- log guest pc +
	// value once per unique value to locate the actual per-frame source.
	if (addr == TA_ISP_LIMIT_addr && cartlog_enabled())
	{
		static u32 seen[16];
		static int nseen;
		int i = 0;
		while (i < nseen && seen[i] != data) i++;
		if (i == nseen && nseen < 16)
		{
			seen[nseen++] = data;
			cartlog("ISPLW val=%08x pc=%08x pr=%08x\n", data,
					p_sh4rcb->cntx.pc, p_sh4rcb->cntx.pr);
		}
	}

	// Cleopatra DreamShell round 12: TA control choreography baseline.
	if ((addr == TA_LIST_INIT_addr || addr == TA_LIST_CONT_addr
	     || addr == SOFTRESET_addr || addr == TA_ALLOC_CTRL_addr
	     || addr == STARTRENDER_addr) && cartlog_enabled())
	{
		cartlog("PVRW %s=%08x\n", regName(paddr), data);
		// Phase 5 Task 5 extension: TEXERR classifier-cell sampler (senkosp) --
		// this is the STARTRENDER write, active on the DC profile every vblank
		// and dynarec-safe (MMIO write dispatch, not interpreter-only). The
		// function throttles itself to every 64th call.
		if (addr == STARTRENDER_addr)
		{
			cartlog_texerr_tick();
			// Phase 5 fix-scoping: arena high-water walker (senkosp) --
			// same site, same dynarec-safety argument; prints only on a
			// new running max.
			cartlog_arena_tick();
			// Phase 5 round-5 (senkosp hw ISTERR bit0 "ISP out of Cache"):
			// render/TA buffer-register snapshot at the render kick, for the
			// Naomi-arm vs DC-arm config diff. Buffer regs alternate every
			// frame (double buffering), so a flat per-render log would be
			// duplicates; instead print each never-seen layout tuple once
			// (FNV hash, 512-slot table) + an unconditional heartbeat every
			// 512 renders so the log shows liveness and render count.
			{
				static u64 rndreg_seen[512];
				static int rndreg_nseen = 0;
				static u32 rndreg_count = 0;
				rndreg_count++;
				const u32 tup[14] = { PARAM_BASE, REGION_BASE, SPAN_SORT_CFG, FPU_PARAM_CFG,
						ISP_FEED_CFG, TA_OL_BASE, TA_ISP_BASE, TA_OL_LIMIT, TA_ISP_LIMIT,
						TA_NEXT_OPB_INIT, TA_GLOB_TILE_CLIP.full, TA_ALLOC_CTRL,
						FB_W_CTRL.full, SCALER_CTL.full };
				u64 h = 0xcbf29ce484222325ULL;
				for (u32 w : tup) { h ^= w; h *= 0x100000001b3ULL; }
				bool fresh = false;
				if (rndreg_nseen < 512) {
					int i = 0;
					while (i < rndreg_nseen && rndreg_seen[i] != h) i++;
					if (i == rndreg_nseen) { rndreg_seen[rndreg_nseen++] = h; fresh = true; }
				}
				// Round-5 poke-site hunt: one-shot full main-RAM dump at render
				// 1500 (game TA layout live, menus). Offline search for the
				// layout words locates the Kamui master copies to shim-poke.
				static bool ramdumped = false;
				if (!ramdumped && rndreg_count == 1500) {
					ramdumped = true;
					const char *clpath = getenv("FLYCAST_CARTLOG");
					if (clpath != nullptr) {
						std::string rpath = std::string(clpath) + ".ram.bin";
						FILE *rf = fopen(rpath.c_str(), "wb");
						if (rf != nullptr) {
							fwrite(&mem_b[0], 1, RAM_SIZE, rf);
							fclose(rf);
							NOTICE_LOG(PVR, "RNDREG ram dump -> %s", rpath.c_str());
						}
					}
				}
				if (fresh || (rndreg_count & 0x1ff) == 0)
					cartlog("RNDREG n=%u %s pb=%08x rb=%08x span=%08x fpu=%08x feed=%08x"
							" olb=%08x ispb=%08x oll=%08x ispl=%08x nopbi=%08x clip=%08x alloc=%08x"
							" wctl=%08x scl=%08x bgd=%08x bgt=%08x nopb=%08x wsof=%08x pc=%08x\n",
							rndreg_count, fresh ? "NEW" : "HB",
							tup[0], tup[1], tup[2], tup[3], tup[4], tup[5], tup[6], tup[7],
							tup[8], tup[9], tup[10], tup[11], tup[12], tup[13],
							ISP_BACKGND_D.i, ISP_BACKGND_T.full, TA_NEXT_OPB, FB_W_SOF1,
							p_sh4rcb->cntx.pc);
			}
		}
	}

	switch (addr)
	{
	case ID_addr:
	case REVISION_addr:
	case TA_YUV_TEX_CNT_addr:
		return; // read only

	case STARTRENDER_addr:
		rend_start_render();
		YUV_init();
		return;

	case TA_LIST_INIT_addr:
		if (data >> 31)
		{
			ta_vtx_ListInit(false);
			TA_NEXT_OPB = TA_NEXT_OPB_INIT;
			TA_ITP_CURRENT = TA_ISP_BASE;
		}
		return;

	case SOFTRESET_addr:
		if (data & 1)
			ta_vtx_SoftReset();
		return;

	case TA_LIST_CONT_addr:
		//a write of anything works ?
		ta_vtx_ListInit(true);
		break;
	
	case SPG_CONTROL_addr:
	case SPG_LOAD_addr:
		if (PvrReg(addr, u32) != data)
		{
			NOTICE_LOG(PVR, "CLEO-SPG write %s = %08x (was %08x) pc=%08x pr=%08x", regName(paddr), data, PvrReg(addr, u32), p_sh4rcb->cntx.pc, p_sh4rcb->cntx.pr);
			PvrReg(addr, u32) = data;
			CalculateSync();
		}
		return;

	case FB_R_CTRL_addr:
		{
			bool vclk_div_changed = (PvrReg(addr, u32) ^ data) & (1 << 23);
			if (PvrReg(addr, u32) != data)
				NOTICE_LOG(PVR, "CLEO-SPG write FB_R_CTRL = %08x (was %08x, vclk_div=%d) pc=%08x pr=%08x", data, PvrReg(addr, u32), (int)((data >> 23) & 1), p_sh4rcb->cntx.pc, p_sh4rcb->cntx.pr);
			PvrReg(addr, u32) = data;
			if (vclk_div_changed)
				CalculateSync();
		}
		return;

	case FB_R_SIZE_addr:
		if (PvrReg(addr, u32) != data)
		{
			NOTICE_LOG(PVR, "CLEO-SPG write FB_R_SIZE = %08x (was %08x) pc=%08x pr=%08x", data, PvrReg(addr, u32), p_sh4rcb->cntx.pc, p_sh4rcb->cntx.pr);
			// Composite mid-bar bug: the load engine (relocated to 8c0a-8c0e at
			// runtime, not present in boot.bin statics) writes the load-era 480i
			// FB_R_SIZE from 8c0db58a but draws the bar with a 240-line layout.
			// One-shot dump of the live engine image for disassembly.
			if (p_sh4rcb->cntx.pc >= 0x8c0d0000 && p_sh4rcb->cntx.pc < 0x8c0e0000
					&& cartlog_enabled()) {
				static bool dumped = false;
				if (!dumped) {
					dumped = true;
					const char *clpath = getenv("FLYCAST_CARTLOG");
					if (clpath != nullptr) {
						std::string path = std::string(clpath) + ".engine.bin";
						FILE *f = fopen(path.c_str(), "wb");
						if (f != nullptr) {
							for (u32 a = 0x8c0a0000; a < 0x8c0e8000; a += 4) {
								u32 v = ReadMem32_nommu(a);
								fwrite(&v, 4, 1, f);
							}
							fclose(f);
							NOTICE_LOG(PVR, "CLEO engine dump 8c0a0000-8c0e8000 -> %s", path.c_str());
						}
					}
				}
			}
			PvrReg(addr, u32) = data;
			fb_dirty = false;
			check_framebuffer_write();
		}
		return;

	case TA_YUV_TEX_BASE_addr:
		PvrReg(addr, u32) = data & 0x00FFFFF8;
		YUV_init();
		return;

	case TA_YUV_TEX_CTRL_addr:
		PvrReg(addr, u32) = data;
		YUV_init();
		return;

	case FB_R_SOF1_addr:
	case FB_R_SOF2_addr:
		data &= 0x00fffffc;
		// DEBUG_LOG: a double-buffered game flips SOF every vblank, so at NOTICE
		// this was ~120 syslog lines/s for the whole run (macOS NSLog is not free)
		if (PvrReg(addr, u32) != data)
			DEBUG_LOG(PVR, "CLEO-SPG write %s = %08x (was %08x) pc=%08x pr=%08x", regName(paddr), data, PvrReg(addr, u32), p_sh4rcb->cntx.pc, p_sh4rcb->cntx.pr);
		// Round 15: HW photo shows scanout (R_SOF 0xB2000) disjoint from the
		// render targets (W_SOF 0x4B2000/0x600000) under DreamShell -- log
		// both sides' write timeline to file (capped) for the flycast
		// baseline comparison.
		if (PvrReg(addr, u32) != data) {
			static int rsof_lines = 0;
			if (rsof_lines < 800) {
				rsof_lines++;
				cartlog("SOFWR %s val=%08x was=%08x pc=%08x pr=%08x\n",
						regName(paddr), data, PvrReg(addr, u32), p_sh4rcb->cntx.pc, p_sh4rcb->cntx.pr);
			}
		}
		// CLEO-VRAMDUMP: FLYCAST_VRAMDUMP=<prefix> -> raw VRAM snapshot every
		// 512 SOF writes (~2-4 s), max 40 files. Offline check of CPU FB paints
		// (loadbar/HUD) that the render path never shows.
		{
			static const char *vd_prefix = getenv("FLYCAST_VRAMDUMP");
			static u32 vd_count, vd_files;
			if (vd_prefix != nullptr && ++vd_count % 512 == 0 && vd_files < 40)
			{
				char vd_path[512];
				snprintf(vd_path, sizeof(vd_path), "%s-%02u.bin", vd_prefix, vd_files++);
				FILE *vd = fopen(vd_path, "wb");
				if (vd != nullptr)
				{
					fwrite(&vram[0], 1, VRAM_SIZE, vd);
					fclose(vd);
					NOTICE_LOG(PVR, "CLEO-VRAMDUMP %s sof1=%08x sof2=%08x fb_r_size=%08x", vd_path, FB_R_SOF1, FB_R_SOF2, FB_R_SIZE.full);
				}
			}
		}
		rend_swap_frame(data);
		break;

	case FB_W_SOF1_addr:
		data &= 0x01fffffc;
		rend_set_fb_write_addr(data);
		if (PvrReg(addr, u32) != data) {
			static int wsof1_lines = 0;
			if (wsof1_lines < 800) {
				wsof1_lines++;
				cartlog("SOFWR FB_W_SOF1 val=%08x was=%08x pc=%08x pr=%08x\n",
						data, PvrReg(addr, u32), p_sh4rcb->cntx.pc, p_sh4rcb->cntx.pr);
			}
		}
		break;

	case FB_W_SOF2_addr:
		data &= 0x01fffffc;
		if (PvrReg(addr, u32) != data) {
			static int wsof2_lines = 0;
			if (wsof2_lines < 800) {
				wsof2_lines++;
				cartlog("SOFWR FB_W_SOF2 val=%08x was=%08x pc=%08x pr=%08x\n",
						data, PvrReg(addr, u32), p_sh4rcb->cntx.pc, p_sh4rcb->cntx.pr);
			}
		}
		break;

	case SPG_HBLANK_INT_addr:
		data &= 0x03FF33FF;
		if (data != SPG_HBLANK_INT.full) {
			SPG_HBLANK_INT.full = data;
			rescheduleSPG();
		}
		return;

	case PAL_RAM_CTRL_addr:
		pal_needs_update = pal_needs_update || ((data ^ PAL_RAM_CTRL) & 3) != 0;
		break;

	case VO_CONTROL_addr:
		// CLEO-VRAMDUMP: a VO_CONTROL write from the shim (0x8cfcxxxx) is the
		// unblank right after a loadbar/hex paint -- snapshot VRAM at exactly
		// that moment (the paint is guaranteed present at the current base).
		{
			static const char *vd_prefix = getenv("FLYCAST_VRAMDUMP");
			static u32 vd_files;
			// loadscreen-dancer-animation final-review residue re-verification
			// (2026-08-04): 20 was too small a capture window to reach the
			// pose-3->4 phrase transition (step 9, ~72 ticks in) -- raised to
			// 400 (docs/kb/tooling.md).
			if (vd_prefix != nullptr && (p_sh4rcb->cntx.pc & 0xffff0000) == 0x8cfc0000 && vd_files < 400)
			{
				char vd_path[512];
				snprintf(vd_path, sizeof(vd_path), "%s-shim-%02u.bin", vd_prefix, vd_files++);
				FILE *vd = fopen(vd_path, "wb");
				if (vd != nullptr)
				{
					fwrite(&vram[0], 1, VRAM_SIZE, vd);
					fclose(vd);
					NOTICE_LOG(PVR, "CLEO-VRAMDUMP %s (shim unblank) sof1=%08x sof2=%08x fb_r_size=%08x pc=%08x", vd_path, FB_R_SOF1, FB_R_SOF2, FB_R_SIZE.full, p_sh4rcb->cntx.pc);
				}
			}
		}
		[[fallthrough]];
	case SPG_HBLANK_addr:
	case SPG_VBLANK_addr:
	case SPG_WIDTH_addr:
	case VO_STARTX_addr:
	case VO_STARTY_addr:
		if (PvrReg(addr, u32) != data)
			NOTICE_LOG(PVR, "CLEO-SPG write %s = %08x (was %08x) pc=%08x pr=%08x", regName(paddr), data, PvrReg(addr, u32), p_sh4rcb->cntx.pc, p_sh4rcb->cntx.pr);
		break;

	default:
		if (addr >= PALETTE_RAM_START_addr && PvrReg(addr,u32) != data)
			pal_needs_update = true;
		else if (addr >= FOG_TABLE_START_addr && addr <= FOG_TABLE_END_addr && PvrReg(addr,u32) != data)
			rend_updateFogTable();
		break;
	}
	PvrReg(addr, u32) = data;
}

void Regs_Reset(bool hard)
{
	if (hard)
		memset(&pvr_regs[0], 0, sizeof(pvr_regs));
	ID_Reg              = 0x17FD11DB;
	REVISION            = 0x00000011;
	SOFTRESET           = 0x00000007;
	SPG_HBLANK_INT.full = 0x031D0000;
	SPG_VBLANK_INT.full = 0x00150104;
	FPU_PARAM_CFG       = 0x0007DF77;
	HALF_OFFSET         = 0x00000007;
	ISP_FEED_CFG        = 0x00402000;
	SDRAM_REFRESH       = 0x00000020;
	SDRAM_ARB_CFG       = 0x0000001F;
	SDRAM_CFG           = 0x15F28997;
	SPG_HBLANK.full     = 0x007E0345;
	SPG_LOAD.full       = 0x01060359;
	SPG_VBLANK.full     = 0x01500104;
	SPG_WIDTH.full      = 0x07F1933F;
	VO_CONTROL.full     = 0x00000108;
	VO_STARTX.full      = 0x0000009D;
	VO_STARTY.full      = 0x00150015;
	SCALER_CTL.full     = 0x00000400;
	FB_BURSTCTRL        = 0x00090639;
	PT_ALPHA_REF        = 0x000000FF;
}
