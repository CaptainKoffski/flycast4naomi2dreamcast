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
#include "sh4_if.h"
#include "sh4_mem.h"
#include "modules/mmu.h"
#include "hw/pvr/pvr_mem.h"
#include "hw/naomi/cartlog.h"

static u32 CCN_QACR_TR[2];

// Cleopatra round 20: SQ writes bypass the addrspace store watch (BANDWR),
// so the game's TA-staging bursts into top-of-RAM were invisible. Log each
// unique 32-byte line touched by SQ in the top 512 KB once (dedup keeps the
// per-frame 0x0cf80000 block from flooding; log position gives the era).
static inline void cartlog_sqband(u32 addr, u32 pc)
{
	u32 pa = addr & 0x1fffffff;
	if (pa < 0x0cf80000 || pa >= 0x0d000000)
		return;
	u32 line = pa & ~0x1fu;
	static u32 seen[512]; static int nseen = 0;
	for (int i = 0; i < nseen; i++)
		if (seen[i] == line) return;
	if (nseen < 512) seen[nseen++] = line;
	cartlog("SQWR line=%08x pc=%08x\n", line, pc);
}

template<bool mmu_on>
static void DYNACALL sqWrite(u32 dest, Sh4Context *ctx)
{
	u32 address;
	//Translate the SQ addresses as needed
	if (mmu_on)
	{
		mmu_TranslateSQW(dest, &address);
	}
	else
	{
		//sanity/optimisation check
		//verify(CCN_QACR_TR[0]==CCN_QACR_TR[1]);

		u32 QACR = CCN_QACR_TR[0];
		//QACR has already 0xE000_0000
		address = QACR + (dest & ~0x1f);
	}

	if (((address >> 26) & 7) != 4)//Area 4
	{
		cartlog_sqband(address, ctx->pc);
		const SQBuffer *sq = &ctx->sq_buffer[(dest >> 5) & 1];
		WriteMemBlock_nommu_sq(address, sq);
	}
	else
	{
		TAWriteSQ(address, ctx->sq_buffer);	// TODO pass the correct SQBuffer instead of letting TAWriteSQ deal with it
	}
}

//yes, this micro optimization makes a difference
static void DYNACALL sqWrite_nommu_area_3(u32 dest, Sh4Context *ctx)
{
	cartlog_sqband(dest, ctx->pc);
	SQBuffer *pmem = (SQBuffer *)((u8 *)ctx + sizeof(Sh4Context) + 0x0C000000);
	pmem += (dest & (RAM_SIZE_MAX - 1)) >> 5;
	*pmem = ctx->sq_buffer[(dest >> 5) & 1];
}

static void DYNACALL sqWrite_nommu_area_3_nonvmem(u32 dest, Sh4Context *ctx)
{
	cartlog_sqband(dest, ctx->pc);
	u8* pmem = &mem_b[0];

	memcpy((SQBuffer *)&pmem[dest & (RAM_MASK - 0x1F)], &ctx->sq_buffer[(dest >> 5) & 1], sizeof(SQBuffer));
}

static void DYNACALL sqWriteTA(u32 dest, Sh4Context *ctx)
{
	TAWriteSQ(dest, ctx->sq_buffer);
}

void setSqwHandler()
{
	Sh4Context& ctx = p_sh4rcb->cntx;
	if (CCN_MMUCR.AT == 1)
	{
		ctx.doSqWrite = &sqWrite<true>;
	}
	else
	{
		u32 area = CCN_QACR0.Area;

		CCN_QACR_TR[0] = (area << 26) - 0xE0000000; //-0xE0000000 because 0xE0000000 is added on the translation again ...

		switch (area)
		{
		case 3:
			if (addrspace::virtmemEnabled())
				ctx.doSqWrite = &sqWrite_nommu_area_3;
			else
				ctx.doSqWrite = &sqWrite_nommu_area_3_nonvmem;
			break;

		case 4:
			ctx.doSqWrite = &sqWriteTA;
			break;

		default:
			ctx.doSqWrite = &sqWrite<false>;
			break;
		}
	}
}
