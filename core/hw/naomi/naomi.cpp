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
#include "hw/aica/aica_if.h"
#include <cstdio>              // Phase 5: snprintf for the ARAM histogram line

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
// (phase2-measurements.md). Fix: zero ARAM once at game handoff (first cart DMA;
// cart DMAs land in MAIN RAM, sound reaches ARAM only later via G2/AICA DMA, so
// nothing is lost), after which any non-zero byte is a genuine game/AICA sound
// write. Report the true high-water + non-zero counts below/above DC's 2 MB, plus
// a 256 KB-bucket histogram so a lone stray write is distinguishable from dense
// usage. Directly answers: does the game's sound data fit DC's 2 MB ARAM?
static void cartlog_aram_profile()
{
	const u8 *ram = &aica::aica_ram[0];
	const u32 size = ARAM_SIZE, BUCK = 0x40000;   // 256 KB buckets
	u32 hist[32] = {0}, nb = size / BUCK;
	if (nb > 32) nb = 32;
	u32 high = 0, nz = 0, nz_below2m = 0;
	for (u32 i = 0; i < size; i++)
		if (ram[i]) {
			nz++; high = i + 1;
			if (i < 0x200000) nz_below2m++;
			u32 b = i / BUCK; if (b < 32) hist[b]++;
		}
	cartlog("ARAMPROFILE high=%x nz=%x nz_below2m=%x nz_above2m=%x size=%x\n",
			high, nz, nz_below2m, nz - nz_below2m, size);
	char line[288]; int p = 0;
	for (u32 b = 0; b < nb; b++)
		p += snprintf(line + p, sizeof(line) - p, "%x ", hist[b]);
	cartlog("ARAMHIST %s\n", line);   // nz-byte count per 256 KB bucket (bucket 8+ = past 2 MB)
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
		// Phase 5: baseline ARAM at game handoff so cartlog_aram_profile measures
		// only genuine game/AICA sound writes (see that fn). First cart DMA = game's
		// first asset fetch; ARAM is still pre-game here (cart DMAs target main RAM).
		static bool aram_zeroed = false;
		if (!aram_zeroed) {
			aram_zeroed = true;
			aica::aica_ram.zero();
			cartlog("ARAMHANDOFF zeroed size=%x\n", ARAM_SIZE);
		}
		static u32 cartlog_dma_count = 0;
		if ((cartlog_dma_count++ & 63) == 0)   // ponytail: every 64th DMA; the scan is cheap but not free
		{
			cartlog_watermarks();
			cartlog_shimwatch();   // Phase 4 (Task 4, V2): shim-home content scan, same cadence
			cartlog_aram_profile();   // Phase 5: sound-RAM fit (write-truth, post-handoff)
		}
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
