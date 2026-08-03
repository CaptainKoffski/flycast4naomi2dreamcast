#include "addrspace.h"
#include "hw/aica/aica_if.h"
#include "hw/pvr/pvr_mem.h"
#include "hw/pvr/elan.h"
#include "hw/sh4/dyna/blockmanager.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/naomi/cartlog.h"   // Phase 4 (Task 13) DC-mode mirror-write instrumentation
#include "oslib/oslib.h"
#include "oslib/virtmem.h"
#include <cassert>
#include <cstdlib>   // getenv (FLYCAST_HWLOG gate)

namespace addrspace
{

#define HANDLER_MAX 0x1F
#define HANDLER_COUNT (HANDLER_MAX+1)

//top registered handler
static handler lastRegisteredHandler;

//handler tables
static ReadMem8FP*   RF8[HANDLER_COUNT];
static WriteMem8FP*  WF8[HANDLER_COUNT];

static ReadMem16FP*  RF16[HANDLER_COUNT];
static WriteMem16FP* WF16[HANDLER_COUNT];

static ReadMem32FP*  RF32[HANDLER_COUNT];
static WriteMem32FP* WF32[HANDLER_COUNT];

//upper 8b of the address
static void* memInfo_ptr[0x100];

#define MAP_RAM_START_OFFSET  0
#define MAP_VRAM_START_OFFSET (MAP_RAM_START_OFFSET+RAM_SIZE)
#define MAP_ARAM_START_OFFSET (MAP_VRAM_START_OFFSET+VRAM_SIZE)
#define MAP_ERAM_START_OFFSET (MAP_ARAM_START_OFFSET+ARAM_SIZE)

void *readConst(u32 addr, bool& ismem, u32 sz)
{
	u32 page = addr >> 24;
	uintptr_t iirf = (uintptr_t)memInfo_ptr[page];
	void *ptr = (void *)(iirf & ~HANDLER_MAX);

	if (ptr == nullptr)
	{
		ismem = false;
		const uintptr_t id = iirf;
		switch (sz)
		{
		case 1:
			return (void *)RF8[id];
		case 2:
			return (void *)RF16[id];
		case 4:
			return (void *)RF32[id];
		default:
			die("Invalid size");
			return nullptr;
		}
	}
	else
	{
		ismem = true;
		addr <<= iirf;
		addr >>= iirf;

		return &(((u8 *)ptr)[addr]);
	}
}

void *writeConst(u32 addr, bool& ismem, u32 sz)
{
	u32 page = addr >> 24;
	uintptr_t iirf = (uintptr_t)memInfo_ptr[page];
	void *ptr = (void *)(iirf & ~HANDLER_MAX);

	if (ptr == nullptr)
	{
		ismem = false;
		const uintptr_t id = iirf;
		switch (sz)
		{
		case 1:
			return (void *)WF8[id];
		case 2:
			return (void *)WF16[id];
		case 4:
			return (void *)WF32[id];
		default:
			die("Invalid size");
			return nullptr;
		}
	}
	else
	{
		ismem = true;
		addr <<= iirf;
		addr >>= iirf;

		return &(((u8 *)ptr)[addr]);
	}
}

// Phase 4 (Task 13): log guest accesses to hardware-register ranges whose
// emulator handlers can loop (holly system/DMA regs, AICA/G2, SH4 on-chip DMAC).
// If the guest is stuck inside a single MMIO opcode (fetch-path PCSAMPLE frozen,
// CPU burning), the LAST HWWR/HWRD line before the freeze is the access that
// entered the looping handler -- i.e. the hang site. pc-2 = the access insn PC
// (interpreter advances ctx->pc to insn+2 before ExecuteOpcode).
static inline void cartlog_hwaccess(char rw, u32 addr, u32 val)
{
	// Opt-in via FLYCAST_HWLOG: the consecutive-dup collapse below misses
	// alternating access pairs (A,B,A,B...), which flooded battery runs at
	// ~230 MB/min of fflushed log (ENOSPC at family 18) while slowing the emu
	// thread. The battery parser never consumes HW lines — debug tool only.
	static const bool hwlog = getenv("FLYCAST_HWLOG") != nullptr;
	if (!hwlog)
		return;
	const u32 pc = Sh4cntx.pc;
	// Only the ported game's own code (post-handoff), matched by PHYSICAL pc so
	// both the P1 (0x8c02xxxx) and P2/uncached (0xac02xxxx) aliases count -- the
	// game dispatches through P2. This filters the loader/KOS boot flood
	// (0x8c00xxxx-0x8c01xxxx) so the log stays bounded and game HW is visible.
	const u32 ppc = pc & 0x1fffffff;
	if (ppc < 0x0c020000 || ppc >= 0x0c200000)
		return;
	if ((addr & 0x1c000000) == 0x0c000000)   // skip main RAM (area 3, incl P1/P2 aliases)
		return;
	// Collapse consecutive identical accesses (a spin logs once, not millions).
	static u32 lpc = 0, laddr = 0; static char lrw = 0;
	if (pc == lpc && addr == laddr && rw == lrw)
		return;
	lpc = pc; laddr = addr; lrw = rw;
	cartlog("HW%c pc=%08x addr=%08x val=%08x\n", rw, pc - 2, addr, val);
}

// Phase 4 (Task 14c): I/O-board decision hunt. The "I/O BD IS NOT CONNECTED"
// status strings live at phys 0x0c0ca6dc..0x0c0ca740 (VA 0x8c0ca6dc, no static
// xref -- computed resource base+offset). Catch the read of that block (the
// screen-build / text-draw), log the reading PC, PR and a heuristic guest-stack
// backtrace (code-looking return addrs) so the decision function that selected
// the string can be traced. Dedup by reading PC so each site logs once.
static inline void cartlog_strwatch(u32 addr)
{
	static const bool hwlog = getenv("FLYCAST_HWLOG") != nullptr;   // see cartlog_hwaccess
	if (!hwlog)
		return;
	const u32 pa = addr & 0x1fffffff;
	if (pa < 0x0c0ca6dc || pa >= 0x0c0ca740)
		return;
	const u32 pc = Sh4cntx.pc;
	static u32 seen[256]; static int nseen = 0;
	for (int i = 0; i < nseen; i++)
		if (seen[i] == pa) return;          // dedup by ADDRESS: see every string byte touched
	if (nseen < 256) seen[nseen++] = pa;
	cartlog("STRWATCH pa=%08x pc=%08x pr=%08x\n", pa, pc - 2, Sh4cntx.pr);
}

template<typename T>
T DYNACALL readt(u32 addr)
{
	constexpr u32 sz = sizeof(T);

	cartlog_hwaccess('R', addr, 0);
	cartlog_strwatch(addr);

	u32 page = addr >> 24;	//1 op, shift/extract
	uintptr_t iirf = (uintptr_t)memInfo_ptr[page]; //2 ops, insert + read [vmem table will be on reg ]
	void *ptr = (void *)(iirf & ~HANDLER_MAX);     //2 ops, and // 1 op insert

	if (likely(ptr != nullptr))
	{
		addr <<= iirf;
		addr >>= iirf;

		return *(T *)&((u8 *)ptr)[addr];
	}
	else
	{
		const u32 id = iirf;
		switch (sz)
		{
		case 1:
			return (T)RF8[id](addr);
		case 2:
			return (T)RF16[id](addr);
		case 4:
			return (T)RF32[id](addr);
		case 8:
			{
				T rv = RF32[id](addr);
				rv |= (T)((u64)RF32[id](addr + 4) << 32);
				return rv;
			}
		default:
			die("Invalid size");
			return 0;
		}
	}
}
template u8 DYNACALL readt<u8>(u32 addr);
template u16 DYNACALL readt<u16>(u32 addr);
template u32 DYNACALL readt<u32>(u32 addr);
template u64 DYNACALL readt<u64>(u32 addr);

template<typename T>
void DYNACALL writet(u32 addr, T data)
{
	constexpr u32 sz = sizeof(T);

	// Phase 4 (Task 13) DC-mode instrumentation: log every guest store into the
	// G1 register-mirror block (phys 0x0cfc8800-0x0cfc9000; the patched game +
	// shim reach it via the P2 alias 0xacfc88xx). Captures the config-time
	// cart-DMA program sequence (cart offset, GDSTAR dest, GDLEN len) and the
	// trigger write of 1 to off 0x418. pc-2 = the store insn's PC: the
	// interpreter advances ctx->pc to insn+2 in ReadNexOp before ExecuteOpcode.
	// ponytail: interpreter-only in practice (dynarec's reg-indirect fast path
	// bypasses writet, per the SHIMWATCH note in naomi.cpp); this pass forces the
	// interpreter so every mirror store is seen. One range compare on the write
	// path -- negligible under the interpreter.
	{
		u32 pa = addr & 0x1fffffff;
		if (pa >= 0x0cfc8800 && pa < 0x0cfc9000)
			cartlog("MIRRORWR pc=%08x off=%03x val=%08x\n",
					Sh4cntx.pc - 2, pa - 0x0cfc8800, (u32)data);
		if (pa == 0x0c0e842c || pa == 0x0c0e6298)
			NOTICE_LOG(SH4, "CLEO-WATCH [%08x] = %08x pc=%08x pr=%08x",
					pa, (u32)data, Sh4cntx.pc - 2, Sh4cntx.pr);
	}
	cartlog_hwaccess('W', addr, (u32)data);

	u32 page = addr>>24;
	uintptr_t iirf = (uintptr_t)memInfo_ptr[page];
	void *ptr = (void *)(iirf & ~HANDLER_MAX);

	if (likely(ptr != nullptr))
	{
		addr <<= iirf;
		addr >>= iirf;

		*(T *)&((u8 *)ptr)[addr] = data;
	}
	else
	{
		const u32 id = iirf;
		switch (sz)
		{
		case 1:
			WF8[id](addr,data);
			break;
		case 2:
			WF16[id](addr,data);
			break;
		case 4:
			WF32[id](addr,data);
			break;
		case 8:
			WF32[id](addr,(u32)data);
			WF32[id](addr+4,(u32)((u64)data>>32));
			break;
		default:
			die("Invalid size");
			break;
		}
	}
}
template void DYNACALL writet<u8>(u32 addr, u8 data);
template void DYNACALL writet<u16>(u32 addr, u16 data);
template void DYNACALL writet<u32>(u32 addr, u32 data);
template void DYNACALL writet<u64>(u32 addr, u64 data);

//ReadMem/WriteMem functions
//ReadMem

u8 DYNACALL read8(u32 Address) { return readt<u8>(Address); }
u16 DYNACALL read16(u32 Address) { return readt<u16>(Address); }
u32 DYNACALL read32(u32 Address) { return readt<u32>(Address); }
u64 DYNACALL read64(u32 Address) { return readt<u64>(Address); }

//WriteMem
void DYNACALL write8(u32 Address,u8 data) { writet<u8>(Address,data); }
void DYNACALL write16(u32 Address,u16 data) { writet<u16>(Address,data); }
void DYNACALL write32(u32 Address,u32 data) { writet<u32>(Address,data); }
void DYNACALL write64(u32 Address,u64 data) { writet<u64>(Address,data); }

#define MEM_ERROR_RETURN_VALUE 0

//default read handler
template<typename T>
static T DYNACALL readMemNotMapped(u32 addresss)
{
	INFO_LOG(MEMORY, "[sh4]read%d from %08x, not mapped (default handler)", (int)sizeof(T), addresss);
	return (T)MEM_ERROR_RETURN_VALUE;
}
//default write hander
template<typename T>
static void DYNACALL writeMemNotMapped(u32 addresss, T data)
{
	INFO_LOG(MEMORY, "[sh4]Write%d to %08x = %x, not mapped (default handler)", (int)sizeof(T), addresss, data);
}

//code to register handlers
//0 is considered error
handler registerHandler(
		ReadMem8FP *read8,
		ReadMem16FP *read16,
		ReadMem32FP *read32,

		WriteMem8FP *write8,
		WriteMem16FP *write16,
		WriteMem32FP *write32)
{
	handler rv = lastRegisteredHandler++;

	assert(rv < HANDLER_COUNT);

	RF8[rv] = read8 == nullptr  ? readMemNotMapped<u8>  : read8;
	RF16[rv] = read16 == nullptr ? readMemNotMapped<u16> : read16;
	RF32[rv] = read32 == nullptr ? readMemNotMapped<u32> : read32;

	WF8[rv] = write8 == nullptr ? writeMemNotMapped<u8> : write8;
	WF16[rv] = write16 == nullptr? writeMemNotMapped<u16> : write16;
	WF32[rv] = write32 == nullptr? writeMemNotMapped<u32> : write32;

	return rv;
}

static u32 FindMask(u32 msk)
{
	u32 s=-1;
	u32 rv=0;

	while(msk!=s>>rv)
		rv++;

	return rv;
}

//map a registered handler to a mem region
void mapHandler(handler Handler, u32 start, u32 end)
{
	assert(start < 0x100);
	assert(end < 0x100);
	assert(start <= end);
	for (u32 i = start; i <= end; i++)
		memInfo_ptr[i] = (u8 *)nullptr + Handler;
}

//map a memory block to a mem region
void mapBlock(void *base, u32 start, u32 end, u32 mask)
{
	assert(start < 0x100);
	assert(end < 0x100);
	assert(start <= end);
	assert((0xFF & (uintptr_t)base) == 0);
	assert(base != nullptr);
	u32 j = 0;
	for (u32 i = start; i <= end; i++)
	{
		memInfo_ptr[i] = &((u8 *)base)[j & mask] + FindMask(mask) - (j & mask);
		j += 0x1000000;
	}
}

void mirrorMapping(u32 new_region, u32 start, u32 size)
{
	u32 end = start + size - 1;
	assert(start < 0x100);
	assert(end < 0x100);
	assert(start <= end);
	assert(!(start >= new_region && end <= new_region));

	u32 j = new_region;
	for (u32 i = start; i <= end; i++)
	{
		memInfo_ptr[j & 0xFF] = memInfo_ptr[i & 0xFF];
		j++;
	}
}

//init/reset/term
void init()
{
	//clear read tables
	memset(RF8, 0, sizeof(RF8));
	memset(RF16, 0, sizeof(RF16));
	memset(RF32, 0, sizeof(RF32));

	//clear write tables
	memset(WF8, 0, sizeof(WF8));
	memset(WF16, 0, sizeof(WF16));
	memset(WF32, 0, sizeof(WF32));

	//clear meminfo table
	memset(memInfo_ptr, 0, sizeof(memInfo_ptr));

	//reset registration index
	lastRegisteredHandler = 0;

	//register default functions (0) for slot 0
	handler defaultHandler = registerHandler(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
	assert(defaultHandler == 0);
	(void)defaultHandler;
}

void term()
{
}

u8* ram_base;

static void *malloc_pages(size_t size)
{
	return allocAligned(PAGE_SIZE, size);
}

static void free_pages(void *p)
{
	freeAligned(p);
}

#if FEAT_SHREC != DYNAREC_NONE

// Resets the FPCB table (by either clearing it to the default val
// or by flushing it and making it fault on access again.
void bm_reset()
{
	// If we allocated it via vmem:
	if (ram_base)
		virtmem::reset_mem(p_sh4rcb->fpcb, sizeof(p_sh4rcb->fpcb));
	else
		// We allocated it via a regular malloc/new/whatever on the heap
		bm_vmem_pagefill((void**)p_sh4rcb->fpcb, sizeof(p_sh4rcb->fpcb));
}

// This gets called whenever there is a pagefault, it is possible that it lands
// on the fpcb memory range, which is allocated on miss. Returning true tells the
// fault handler this was us, and that the page is resolved and can continue the execution.
bool bm_lockedWrite(u8* address)
{
	if (!ram_base)
		return false;  // No vmem, therefore not us who caused this.

	uintptr_t ptrint = (uintptr_t)address;
	uintptr_t start  = (uintptr_t)p_sh4rcb->fpcb;
	uintptr_t end    = start + sizeof(p_sh4rcb->fpcb);

	if (ptrint >= start && ptrint < end)
	{
		// Alloc the page then and initialize it to default values
		void *aligned_addr = (void*)(ptrint & (~PAGE_MASK));
		virtmem::ondemand_page(aligned_addr, PAGE_SIZE);
		bm_vmem_pagefill((void**)aligned_addr, PAGE_SIZE);
		return true;
	}
	return false;
}
#endif

bool reserve()
{
	if (ram_base != nullptr)
		return true;

	virtmem::init((void**)&ram_base, (void**)&p_sh4rcb, RAM_SIZE_MAX + VRAM_SIZE_MAX + ARAM_SIZE_MAX + elan::ERAM_SIZE_MAX);
	return true;
}

static void termMappings()
{
	if (ram_base == nullptr)
	{
		free_pages(p_sh4rcb);
		p_sh4rcb = nullptr;
		mem_b.free();
		vram.free();
		aica::aica_ram.free();
		free_pages(elan::RAM);
		elan::RAM = nullptr;
	}
}

void initMappings()
{
	termMappings();
	// Fallback to statically allocated buffers, this results in slow-ops being generated.
	if (ram_base == nullptr)
	{
		WARN_LOG(VMEM, "Warning! nvmem is DISABLED (due to failure or not being built-in");

		// Allocate it all and initialize it.
		p_sh4rcb = (Sh4RCB*)malloc_pages(sizeof(Sh4RCB));
#if FEAT_SHREC != DYNAREC_NONE
		bm_vmem_pagefill((void**)p_sh4rcb->fpcb, sizeof(p_sh4rcb->fpcb));
#endif
		memset(&p_sh4rcb->cntx, 0, sizeof(p_sh4rcb->cntx));

		mem_b.alloc(RAM_SIZE);
		vram.alloc(VRAM_SIZE);
		aica::aica_ram.alloc(ARAM_SIZE);
		elan::RAM = (u8*)malloc_pages(elan::ERAM_SIZE);
	}
	else {
		NOTICE_LOG(VMEM, "Info: nvmem is enabled");
		INFO_LOG(VMEM, "Info: p_sh4rcb: %p ram_base: %p", p_sh4rcb, ram_base);
		// Map the different parts of the memory file into the new memory range we got.
		const virtmem::Mapping mem_mappings[] = {
			{0x00000000, 0x00800000,                               0,         0, false},  // Area 0 -> unused
			{0x00800000, 0x01000000,           MAP_ARAM_START_OFFSET, ARAM_SIZE, false},  // Aica
			{0x01000000, 0x04000000,                               0,         0, false},  // More unused
			{0x04000000, 0x05000000,           MAP_VRAM_START_OFFSET, VRAM_SIZE,  true},  // Area 1 (vram, 16MB, wrapped on DC as 2x8MB)
			{0x05000000, 0x06000000,                               0,         0, false},  // 32 bit path (unused)
			{0x06000000, 0x07000000,           MAP_VRAM_START_OFFSET, VRAM_SIZE,  true},  // VRAM mirror
			{0x07000000, 0x08000000,                               0,         0, false},  // 32 bit path (unused) mirror
			{0x08000000, 0x0A000000,                               0,         0, false},  // Area 2
			{0x0A000000, 0x0C000000,           MAP_ERAM_START_OFFSET, elan::ERAM_SIZE, true},  // Area 2 (Elan RAM)
			{0x0C000000, 0x10000000,            MAP_RAM_START_OFFSET,  RAM_SIZE,  true},  // Area 3 (main RAM + 3 mirrors)
			{0x10000000, 0x20000000,                               0,         0, false},  // Area 4-7 (unused)
			// This is outside of the 512MB addr space. We map 8MB in all cases to help some games read past the end of aica ram
			{0x20000000, 0x20800000,           MAP_ARAM_START_OFFSET, ARAM_SIZE,  true},  // writable aica ram
		};
		virtmem::create_mappings(&mem_mappings[0], std::size(mem_mappings));

		// Point buffers to actual data pointers
		// This *must* be the first r/w mirror (switch)
		aica::aica_ram.setRegion(&ram_base[0x20000000], ARAM_SIZE); // Points to the writable AICA addrspace
		vram.setRegion(&ram_base[0x04000000], VRAM_SIZE); // Points to first vram mirror (writable and lockable)
		mem_b.setRegion(&ram_base[0x0C000000], RAM_SIZE); // Main memory, first mirror
		elan::RAM = &ram_base[0x0A000000];
	}

	// Clear out memory
	aica::aica_ram.zero();
	vram.zero();
	mem_b.zero();
	NOTICE_LOG(VMEM, "BASE %p RAM(%d MB) %p VRAM64(%d MB) %p ARAM(%d MB) %p",
			ram_base,
			(u32)(RAM_SIZE / 1_MB), &mem_b[0],
			(u32)(VRAM_SIZE / 1_MB), &vram[0],
			(u32)(ARAM_SIZE / 1_MB), &aica::aica_ram[0]);
}

void release()
{
	if (ram_base != nullptr)
	{
		virtmem::destroy();
		ram_base = nullptr;
	}
	else
	{
		unprotectVram(0, VRAM_SIZE);
		termMappings();
	}
}

void protectVram(u32 addr, u32 size)
{
	addr &= VRAM_MASK;
#ifndef __SWITCH__
	if (virtmemEnabled())
	{
		virtmem::region_lock(ram_base + 0x04000000 + addr, size);	// P0
		//virtmem::region_lock(ram_base + 0x06000000 + addr, size);	// P0 - mirror
		if (VRAM_SIZE == 0x800000)
		{
			// wraps when only 8MB VRAM
			virtmem::region_lock(ram_base + 0x04000000 + addr + VRAM_SIZE, size);	// P0 wrap
			//virtmem::region_lock(ram_base + 0x06000000 + addr + VRAM_SIZE, size);	// P0 mirror wrap
		}
	}
	else
#endif
	{
		virtmem::region_lock(&vram[addr], size);
	}
}

void unprotectVram(u32 addr, u32 size)
{
	addr &= VRAM_MASK;
#ifndef __SWITCH__
	if (virtmemEnabled())
	{
		virtmem::region_unlock(ram_base + 0x04000000 + addr, size);		// P0
		//virtmem::region_unlock(ram_base + 0x06000000 + addr, size);	// P0 - mirror
		if (VRAM_SIZE == 0x800000)
		{
			// wraps when only 8MB VRAM
			virtmem::region_unlock(ram_base + 0x04000000 + addr + VRAM_SIZE, size);		// P0 wrap
			//virtmem::region_unlock(ram_base + 0x06000000 + addr + VRAM_SIZE, size);	// P0 mirror wrap
		}
	}
	else
#endif
	{
		virtmem::region_unlock(&vram[addr], size);
	}
}

u32 getVramOffset(void *addr)
{
#ifndef __SWITCH__
	if (virtmemEnabled())
	{
		ptrdiff_t offset = (u8*)addr - ram_base;
		if (offset < 0 || offset >= 0x20000000)
			return -1;
		if ((offset >> 24) != 4)
			return -1;

		return offset & VRAM_MASK;
	}
	else
#endif
	{
		ptrdiff_t offset = (u8*)addr - &vram[0];
		if (offset < 0 || offset >= VRAM_SIZE)
			return -1;

		return (u32)offset;
	}
}

void getAddress(void** out_ram_base, void** out_ram, void** out_vram, void** out_aica) {
    if (ram_base != nullptr) {
        *out_ram_base =	ram_base;
        *out_ram = &mem_b[0];
        *out_vram = &vram[0];
        *out_aica = &aica::aica_ram[0];
    } else {
        *out_ram_base =	nullptr;
        *out_ram = nullptr;
        *out_vram = nullptr;
        *out_aica = nullptr;
    }
}

} // namespace addrspace
