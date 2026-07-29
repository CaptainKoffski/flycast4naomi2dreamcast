/*
	Highly inefficient and boring interpreter. Nothing special here
*/

#include "types.h"
#include "hw/naomi/cartlog.h"   // Phase 3 instrumentation
#include <ctime>                // Phase 4 (Task 13): wall-clock PC sampler

#include "../sh4_interpreter.h"
#include "../sh4_opcode_list.h"
#include "../sh4_core.h"
#include "../sh4_interrupts.h"
#include "hw/sh4/sh4_mem.h"
#include "../sh4_sched.h"
#include "../sh4_cache.h"
#include "debug/gdb_server.h"
#include "../sh4_cycles.h"

Sh4ICache icache;
Sh4OCache ocache;
Sh4Interpreter *Sh4Interpreter::Instance;

// Phase 3: flag any guest execution inside BIOS ROM (phys < 0x200000) AFTER the
// Naomi entrypoint is first reached. Dynamic half of naomi-vs-dreamcast §8-3.
// ponytail: interpreter-only (this pass forces the interpreter); dynarec won't fire this.
static bool cartlog_entry_seen = false;
static void cartlog_bios_check(u32 pc)
{
	if (pc == 0x8c04ae2c)
		cartlog_entry_seen = true;
	if (cartlog_entry_seen && (pc & 0x1fffffff) < 0x00200000)
	{
		static u32 last = 0xffffffff;
		if (pc != last) { last = pc; cartlog("BIOSEXEC pc=%08x\n", pc); }
	}
}

// Phase 4 (Task 13): busy-spin PC detector. If the guest stays inside a <32-byte
// PC window for >50M consecutive instruction fetches it is busy-spinning (a
// `bf`-to-self DMA-completion poll); emit the spin PC + the mirrored SB_GDST slot
// mirror+0x418 (phys 0x0cfc8c18) once. 50M is far above any bounded delay/init
// loop (biggest is ~1M iters) but is crossed near-instantly by a real busy spin.
// A paced SLEEP/interrupt-driven idle is NOT a busy spin (fetch count barely
// climbs) -- that case is caught by PCSAMPLE in Run() below and SLEEPWAIT.
// ponytail: interpreter-only (pc is instruction-exact here); dynarec won't fire this.
static void cartlog_hang_check(u32 pc)
{
	// Phase 4 (Task 13): wall-clock guest-PC sampler, in the instruction-fetch
	// path. Logs the fetched PC once per real second. Placed here (not in Run()'s
	// outer timeslice loop) because a heavily real-time-paced inner loop reaches
	// the outer loop only rarely -- an outer-loop sampler starves. As long as any
	// instruction is fetched each second, this pins the stuck PC regardless of
	// pacing. gdst_mirror = mirror+0x418 at the sample.
	{
		static time_t last = 0;
		time_t now = time(nullptr);
		if (now != last)
		{
			last = now;
			cartlog("PCSAMPLE pc=%08x gdst_mirror=%08x\n", pc,
					ReadMem32_nommu(0x8cfc8c18));
		}
	}

	static u32 win_base = 0;
	static u64 count = 0;
	static bool done = false;
	if (done)
		return;
	if ((u32)(pc - win_base) < 32)
	{
		if (++count > 50000000ull)
		{
			cartlog("HANG pc=%08x gdst_mirror=%08x\n", pc,
					ReadMem32_nommu(0x8cfc8c18));
			done = true;
		}
	}
	else
	{
		win_base = pc;
		count = 0;
	}
}

void Sh4Interpreter::ExecuteOpcode(u16 op)
{
	if (ctx->sr.FD == 1 && OpDesc[op]->IsFloatingPoint())
		throw SH4ThrownException(ctx->pc - 2, Sh4Ex_FpuDisabled);
	OpPtr[op](ctx, op);
	sh4cycles.executeCycles(op);
}

// Phase 4 (Task 14c): I/O-board decision probe. FUN_8c04ae50 (scene loop) tests
// the I/O-check scene at a few PCs. Snapshot the scene object (*0x8c0c4510), its
// vtable methods (+0x10 "scene finished?", +0x7c "per-frame done?"), and the
// enumeration flags (conn 0x8c1c9774, specs 0x8c0d541c, mirror 0x8c127b0c) plus
// r0 (the just-returned method value) so the true gate method can be disassembled.
static void cartlog_iocheck(u32 pc)
{
	u32 ppc = pc & 0x1fffffff;
	if (ppc != 0x0c04b08a && ppc != 0x0c04b090 && ppc != 0x0c04b1fa && ppc != 0x0c04b200)
		return;
	static int n = 0; if (n > 60) return; n++;
	u32 obj = ReadMem32_nommu(0x8c0c4510);
	cartlog("IOCHK pc=%08x r0=%08x obj=%08x m10=%08x m7c=%08x conn=%08x specs=%08x mir=%08x\n",
			ppc, Sh4cntx.r[0], obj,
			obj ? ReadMem32_nommu(obj + 0x10) : 0, obj ? ReadMem32_nommu(obj + 0x7c) : 0,
			ReadMem32_nommu(0x8c1c9774), ReadMem32_nommu(0x8c0d541c), ReadMem32_nommu(0x8c127b0c));
}

u16 Sh4Interpreter::ReadNexOp()
{
	u32 addr = ctx->pc;
	cartlog_bios_check(addr);
	cartlog_hang_check(addr);   // Phase 4 (Task 13): boot-hang spin-PC detector
	cartlog_iocheck(addr);      // Phase 4 (Task 14c): I/O-board decision probe
	if (!mmu_enabled() && (addr & 1))
		// address error
		throw SH4ThrownException(addr, Sh4Ex_AddressErrorRead);

	ctx->pc = addr + 2;

	return IReadMem16(addr);
}

void Sh4Interpreter::Run()
{
	Instance = this;
	ctx->restoreHostRoundingMode();

	try {
		do
		{
			try {
				do
				{
					u32 op = ReadNexOp();

					ExecuteOpcode(op);
				} while (ctx->cycle_counter > 0);
				ctx->cycle_counter += SH4_TIMESLICE;
				UpdateSystem_INTC();
			} catch (const SH4ThrownException& ex) {
				Do_Exception(ex.epc, ex.expEvn);
				// an exception requires the instruction pipeline to drain, so approx 5 cycles
				sh4cycles.addCycles(5 * CPU_RATIO);
				// Phase 4 (Task 13): an infinite exception loop (a fault taken every
				// instruction -- e.g. before the guest's VBR handlers are installed)
				// exits the inner loop straight to here each iteration, bypassing the
				// PCSAMPLE above. Log the faulting PC (epc), event code (expEvn: 0x180
				// illegal-instr, 0x1a0 slot-illegal, 0x0e0/0x100 data address error,
				// etc.) and the vector jumped to. Wall-clock rate-limited (~1/sec).
				{
					static time_t last = 0;
					time_t now = time(nullptr);
					if (now != last)
					{
						last = now;
						cartlog("EXC epc=%08x evn=%03x newpc=%08x vbr=%08x\n",
								ex.epc, ex.expEvn, ctx->pc, ctx->vbr);
					}
				}
			}
		} while (ctx->CpuRunning);
	} catch (const debugger::Stop&) {
	}

	ctx->CpuRunning = false;
	Instance = nullptr;
}

void Sh4Interpreter::Start()
{
	ctx->CpuRunning = true;
}

void Sh4Interpreter::Stop()
{
	ctx->CpuRunning = false;
	ctx->cycle_counter = 0;
}

void Sh4Interpreter::Step()
{
	Instance = this;

	ctx->restoreHostRoundingMode();
	try {
		u32 op = ReadNexOp();
		ExecuteOpcode(op);
	} catch (const SH4ThrownException& ex) {
		Do_Exception(ex.epc, ex.expEvn);
		// an exception requires the instruction pipeline to drain, so approx 5 cycles
		sh4cycles.addCycles(5 * CPU_RATIO);
	} catch (const debugger::Stop&) {
	}
	Instance = nullptr;
}

void Sh4Interpreter::Reset(bool hard)
{
	verify(!ctx->CpuRunning);

	if (hard)
	{
		int schedNext = ctx->sh4_sched_next;
		memset(ctx, 0, sizeof(*ctx));
		ctx->sh4_sched_next = schedNext;
	}
	ctx->pc = 0xA0000000;

	memset(ctx->r, 0, sizeof(ctx->r));
	memset(ctx->r_bank, 0, sizeof(ctx->r_bank));

	ctx->gbr = ctx->ssr = ctx->spc = ctx->sgr = ctx->dbr = ctx->vbr = 0;
	ctx->mac.full = ctx->pr = ctx->fpul = 0;

	ctx->sr.setFull(0x700000F0);
	ctx->old_sr.status = ctx->sr.status;
	UpdateSR();

	ctx->fpscr.full = 0x00040001;
	ctx->old_fpscr = ctx->fpscr;

	icache.Reset(hard);
	ocache.Reset(hard);
	sh4cycles.reset();
	ctx->cycle_counter = SH4_TIMESLICE;

	INFO_LOG(INTERPRETER, "Sh4 Reset");
}

bool Sh4Interpreter::IsCpuRunning()
{
	return ctx->CpuRunning;
}

//TODO : Check for valid delayslot instruction
void Sh4Interpreter::ExecuteDelayslot()
{
	try {
		u32 op = ReadNexOp();

		ExecuteOpcode(op);
	} catch (SH4ThrownException& ex) {
		AdjustDelaySlotException(ex);
		throw ex;
	} catch (const debugger::Stop& e) {
		ctx->pc -= 2;	// break on previous instruction
		throw e;
	}
}

void Sh4Interpreter::ExecuteDelayslot_RTE()
{
	try {
		// In an RTE delay slot, status register (SR) bits are referenced as follows.
		// In instruction access, the MD bit is used before modification, and in data access,
		// the MD bit is accessed after modification.
		// The other bits—S, T, M, Q, FD, BL, and RB—after modification are used for delay slot
		// instruction execution. The STC and STC.L SR instructions access all SR bits after modification.
		u32 op = ReadNexOp();
		// Now restore all SR bits
		ctx->sr.setFull(ctx->ssr);
		// And execute
		ExecuteOpcode(op);
	} catch (const SH4ThrownException&) {
		throw FlycastException("Fatal: SH4 exception in RTE delay slot");
	} catch (const debugger::Stop& e) {
		ctx->pc -= 2;	// break on previous instruction
		throw e;
	}
}

// every SH4_TIMESLICE cycles
int UpdateSystem_INTC()
{
	Sh4cntx.sh4_sched_next -= SH4_TIMESLICE;
	if (Sh4cntx.sh4_sched_next < 0)
		sh4_sched_tick(SH4_TIMESLICE);
	if (Sh4cntx.interrupt_pend)
		return UpdateINTC();
	else
		return 0;
}

void Sh4Interpreter::Init()
{
	ctx = &p_sh4rcb->cntx;
	memset(ctx, 0, sizeof(*ctx));
	sh4cycles.init(ctx);
	icache.init(ctx);
	ocache.init(ctx);
}

void Sh4Interpreter::Term()
{
	Stop();
	INFO_LOG(INTERPRETER, "Sh4 Term");
}

Sh4Executor *Get_Sh4Interpreter()
{
	return new Sh4Interpreter();
}
