#include "maple_if.h"
#include "maple_cfg.h"
#include "maple_helper.h"
#include "hw/holly/holly_intc.h"
#include "hw/holly/sb.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/sh4/sh4_sched.h"
#include "network/ggpo.h"
#include "hw/naomi/card_reader.h"
#include "hw/naomi/cartlog.h"   // Phase 4 (Task 4, V4) instrumentation

#include <memory>
#include <cstring>

enum MaplePattern
{
	MP_Start,
	MP_SDCKBOccupy = 2,
	MP_Reset,
	MP_SDCKBOccupyCancel,
	MP_NOP = 7
};

std::shared_ptr<maple_device> MapleDevices[MAPLE_PORTS][6];

int maple_schid;

/*
	Maple host controller
	Direct processing, async interrupt handling
	Device code is on maple_devs.cpp/h, config&management is on maple_cfg.cpp/h

	This code is missing many of the hardware details, like proper trigger handling,
	DMA continuation on suspect, etc ...
*/

static void maple_DoDma();
static void maple_handle_reconnect();
static int maple_schd(int tag, int cycles, int jitter, void *arg);

//really hackish
//misses delay , and stop/start implementation
//ddt/etc are just hacked for wince to work
//now with proper maple delayed DMA maybe its time to look into it ?
bool maple_ddt_pending_reset;
// pending DMA xfers
std::vector<std::pair<u32, std::vector<u32>>> mapleDmaOut;
bool SDCKBOccupied;

// Phase 4 (Task 1): which caller reached maple_DoDma() for the in-flight
// transaction. Set at each of the two call sites below, read by maple_DoDma()
// and by maple_jvs.cpp via maple_getTrig() -- boot-binary.md "Why three
// checks cannot pass as written": a register-triggered transaction carries an
// attributable guest-store PC; a vblank-triggered one does not.
static const char *maple_trig = "?";

const char *maple_getTrig() { return maple_trig; }

void maple_vblank()
{
	if (SB_MDEN & 1)
	{
		if (SB_MDTSEL == 1)
		{
			// Hardware trigger on vblank
			if (maple_ddt_pending_reset) {
				DEBUG_LOG(MAPLE, "DDT vblank ; reset pending");
			}
			else
			{
				//DEBUG_LOG(MAPLE, "DDT vblank");
				maple_trig = "vbl";   // Phase 4 (Task 1): hardware vblank trigger, no guest store
				SB_MDST = 1;
				maple_DoDma();
				// if trigger reset is manual, mark it as pending
				if ((SB_MSYS >> 12) & 1)
					maple_ddt_pending_reset = true;
			}
		}
		else
		{
			maple_ddt_pending_reset = false;
			if (SDCKBOccupied)
				maple_schd(0, 0, 0, nullptr);
		}
		SDCKBOccupied = false;
	}
	if (settings.platform.isConsole())
		maple_handle_reconnect();
}

static void maple_SB_MSHTCL_Write(u32 addr, u32 data)
{
	if (data & 1)
		maple_ddt_pending_reset = false;
}

static void maple_SB_MDST_Write(u32 addr, u32 data)
{
	if (data & 1)
	{
		if (SB_MDEN & 1)
		{
			SB_MDST = 1;
			maple_trig = "reg";   // Phase 4 (Task 1): guest SB_MDST store, attributable PC
			maple_DoDma();
		}
	}
}

static void maple_SB_MDEN_Write(u32 addr, u32 data)
{
	SB_MDEN = data & 1;

	if ((data & 1) == 0 && SB_MDST)
		INFO_LOG(MAPLE, "Maple DMA abort ?");
}

#ifdef STRICT_MODE
static bool check_mdapro(u32 addr)
{
	u32 area = (addr >> 26) & 7;
	u32 bottom = ((((SB_MDAPRO >> 8) & 0x7f) << 20) | 0x08000000);
	u32 top = (((SB_MDAPRO & 0x7f) << 20) | 0x080fffe0);

	if (area != 3 || addr < bottom || addr > top)
	{
		INFO_LOG(MAPLE, "MAPLE ERROR : Invalid address: %08x. SB_MDAPRO: %x %x", addr, (SB_MDAPRO >> 8) & 0x7f, SB_MDAPRO & 0x7f);
		return false;
	}
	return true;
}

static void maple_SB_MDSTAR_Write(u32 addr, u32 data)
{
	SB_MDSTAR = data & 0x1fffffe0;
	if (!check_mdapro(SB_MDSTAR))
		asic_RaiseInterrupt(holly_MAPLE_ILLADDR);
}
#endif

static u32 getPort(u32 addr)
{
	for (int i = 0; i < 6; i++)
		if ((1 << i) & addr)
			return i;
	return 5;
}

static void maple_DoDma()
{
	verify(SB_MDEN & 1);
	verify(SB_MDST & 1);

	DEBUG_LOG(MAPLE, "Maple: DoMapleDma SB_MDSTAR=%x", SB_MDSTAR);
	u32 addr = SB_MDSTAR;
#ifdef STRICT_MODE
	if (!check_mdapro(addr))
	{
		asic_RaiseInterrupt(holly_MAPLE_ILLADDR);
		SB_MDST = 0;
		return;
	}
#endif

	ggpo::getInput(mapleInputState);
	// TODO put this elsewhere and let the card readers handle being called multiple times
	if (settings.platform.isNaomi())
	{
		static u32 last_kcode[std::size(mapleInputState)];
		for (size_t i = 0; i < std::size(mapleInputState); i++)
		{
			if ((mapleInputState[i].kcode & DC_BTN_INSERT_CARD) == 0
					&& (last_kcode[i] & DC_BTN_INSERT_CARD) != 0)
				card_reader::insertCard(i);
			last_kcode[i] = mapleInputState[i].kcode;
		}
	}

	const bool swap_msb = (SB_MMSEL == 0);
	u32 xferOut = 0;
	u32 xferIn = 0;
	bool last = false;
	// Phase 4 (Task 13): DC-mode boot-hang capture. maple_DoDma runs SYNCHRONOUSLY
	// from the guest's SB_MDST=1 store and walks a command list from SB_MDSTAR with
	// an unbounded `while(!last)` loop (last = command header bit31). A list whose
	// terminator is never reached makes this loop walk RAM/MMIO forever, freezing
	// the guest inside that single store opcode -- exactly the observed hang
	// (fetch-path PCSAMPLE frozen, CPU burning). Log entry + guard the loop.
	// Phase 4 (Task 1): trig tag (this call's trigger source) + r15 water-mark
	// sample -- MDODMA enter fires once per maple_DoDma() call, at many
	// different task call-depths, unlike the single fixed depth CARTDMAPC
	// samples SP at (see cartlog.h).
	cartlog_sp_sample(Sh4cntx.r[15]);
	cartlog("MDODMA enter mdstar=%08x hdr0=%08x mden=%d pc=%08x trig=%s\n",
			SB_MDSTAR, ReadMem32_nommu(SB_MDSTAR), SB_MDEN & 1, Sh4cntx.pc, maple_trig);
	u32 mdodma_iters = 0;
	while (!last)
	{
		if (++mdodma_iters > 100000)
		{
			cartlog("MDODMA_RUNAWAY iters=%u addr=%08x hdr1=%08x\n",
					mdodma_iters, addr, ReadMem32_nommu(addr));
			SB_MDST = 0;          // escape: clear busy so the guest's poll exits; see where it goes next
			mapleDmaOut.clear();
			return;
		}
		u32 header_1 = ReadMem32_nommu(addr);
		u32 header_2 = ReadMem32_nommu(addr + 4) & 0x1FFFFFE0;

		last = (header_1 >> 31) == 1;				// is last transfer ?
		u32 plen = (header_1 & 0xFF) + 1;			// transfer length (32-bit unit)
		const u32 maple_op = (header_1 >> 8) & 7;	// Pattern selection: 0 - START, 2 - SDCKB occupy permission, 3 - RESET, 4 - SDCKB occupy cancel, 7 - NOP
		const u32 bus = (header_1 >> 16) & 3;		// maple bus [0..3]

		//this is kinda wrong .. but meh
		//really need to properly process the commands at some point
		switch (maple_op)
		{
		case MP_Start:
		{
#ifdef STRICT_MODE
			if (!check_mdapro(header_2) || !check_mdapro(addr + (2 + plen) * sizeof(u32) - 1))
			{
#else
			if (GetMemPtr(header_2, 1) == nullptr)
			{
				INFO_LOG(MAPLE, "DMA Error: destination not in system ram: %x", header_2);
#endif
				header_2 = 0;
			}

			u32* p_data = (u32 *)GetMemPtr(addr + 8, plen * sizeof(u32));
			if (p_data == nullptr)
			{
				WARN_LOG(MAPLE, "MAPLE ERROR : INVALID SB_MDSTAR value 0x%X", addr);
				SB_MDST = 0;
				mapleDmaOut.clear();
				return;
			}
			const u32 frame_header = swap_msb ? SWAP32(p_data[0]) : p_data[0];

			//Command code
			const u32 command = frame_header & 0xFF;
			//Recipient address
			const u32 reci = (frame_header >> 8) & 0xFF;//0-5;
			//Sender address
			//u32 send = (frame_header >> 16) & 0xFF;
			//Number of additional words in frame
			//u32 inlen = (frame_header >> 24) & 0xFF;

			u32 port = 5;
			// If the connected device doesn't have expansion ports, ignore the message header
			// and send everything to the main device.
			auto pDevice = MapleDevices[bus][5];
			if (pDevice != nullptr
					&& maple_getPortCount(pDevice->get_device_type()) != 0)
			{
				port = getPort(reci);
				if (port != 5)
					pDevice = MapleDevices[bus][port];
			}

			if (pDevice != nullptr)
			{
				if (swap_msb)
				{
					static u32 maple_in_buf[1024 / sizeof(u32)];
					maple_in_buf[0] = frame_header;
					for (u32 i = 1; i < plen; i++)
						maple_in_buf[i] = SWAP32(p_data[i]);
					p_data = maple_in_buf;
				}
				u32 outbuf[1024 / sizeof(u32)];
				// Phase 4 (Task 4, V4): zero first so the fixed 0x40-byte MIERESP dump
				// below never exposes uninitialized stack bytes past outlen.
				memset(outbuf, 0, sizeof(outbuf));
				// Phase 4 (Task 13): bracket RawDma + the reply-vector build to pin a
				// hang inside a single Maple frame (RawDma handler or a bogus huge
				// outlen -> gigabyte std::vector). "call" with no "ret" => RawDma hung;
				// "ret" with a huge outlen => the emplace_back below is the hang.
				cartlog("MDODMA rawdma_call cmd=%02x reci=%02x bus=%d plen=%u\n",
						frame_header & 0xFF, (frame_header >> 8) & 0xFF, bus, plen);
				u32 outlen = pDevice->RawDma(&p_data[0], plen * sizeof(u32), outbuf);
				cartlog("MDODMA rawdma_ret outlen=%x\n", outlen);
				xferIn += plen * sizeof(u32) + 3; // start, parity and stop bytes
				xferOut += outlen + 3;
#ifdef STRICT_MODE
				if (!check_mdapro(header_2 + outlen - 1))
				{
					asic_RaiseInterrupt(holly_MAPLE_OVERRUN);
					SB_MDST = 0;
					mapleDmaOut.clear();
					return;
				}
#endif
				if (swap_msb)
					for (u32 i = 0; i < outlen / 4; i++)
						outbuf[i] = SWAP32(outbuf[i]);
				// Phase 4 (Task 4, V4) instrumentation: dump the MIE's actual reply
				// bytes + destination (recv) address for JVS I/O transactions (cmd
				// 0x86), read AFTER RawDma has produced the response (and after the
				// swap_msb fixup above) so this is byte-identical to what gets
				// written to guest RAM at header_2 below -- response, not request.
				if (command == MDC_JVSCommand && outlen > 0)
				{
					u8 sub = ((const u8 *)p_data)[4];
					cartlog("MIERESP sub=%02x addr=%08x data=", sub, header_2);
					const u8 *resp = (const u8 *)outbuf;
					for (int i = 0; i < 0x40; i++)
						cartlog("%02x", resp[i]);
					cartlog(" trig=%s\n", maple_trig);   // Phase 4 (Task 1)
				}
				mapleDmaOut.emplace_back(header_2, std::vector<u32>(outbuf, outbuf + outlen / 4));
				cartlog("MDODMA frame_done outlen=%x\n", outlen);   // Phase 4 (Task 13)
			}
			else
			{
				if (port != 5 && command != 1)
					INFO_LOG(MAPLE, "MAPLE: Unknown device bus %d port %d cmd %d reci %d", bus, port, command, reci);
				mapleDmaOut.emplace_back(header_2, std::vector<u32>(1, 0xFFFFFFFF));
			}

			//goto next command
			addr += (2 + plen) * sizeof(u32);
		}
		break;

		case MP_SDCKBOccupy:
		{
			u32 bus = (header_1 >> 16) & 3;
			auto pDevice = MapleDevices[bus][5];
			if (pDevice) {
				SDCKBOccupied = SDCKBOccupied || pDevice->get_lightgun_pos();
				xferIn++;
			}
			addr += 1 * sizeof(u32);
		}
		break;

		case MP_SDCKBOccupyCancel:
			SDCKBOccupied = false;
			addr += 1 * sizeof(u32);
			break;

		case MP_Reset:
			addr += 1 * sizeof(u32);
			xferIn++;
			break;

		case MP_NOP:
			addr += 1 * sizeof(u32);
			break;

		default:
			INFO_LOG(MAPLE, "MAPLE: Unknown maple_op == %d length %d", maple_op, plen * 4);
			addr += 1 * sizeof(u32);
		}
	}

	// Maple bus max speed: 2 Mb/s, actual speed: 1 Mb/s
	// actual measured speed with protocol analyzer for devices (vmu?) is 724-738Kb/s
	// See https://github.com/OrangeFox86/DreamcastControllerUsbPico/blob/main/measurements/Dreamcast-Power-Up-Digital-and-Analog-Player1-Controller-VMU-JumpPack.sal
	if (!SDCKBOccupied)
	{
		// 2 Mb/s from console
		u32 cycles = sh4CyclesForXfer(xferIn, 2'000'000 / 8);
		// 740 Kb/s from devices
		cycles += sh4CyclesForXfer(xferOut, 740'000 / 8);
		cycles = std::min<u32>(cycles, SH4_MAIN_CLOCK);
		sh4_sched_request(maple_schid, cycles);
	}
}

static int maple_schd(int tag, int cycles, int jitter, void *arg)
{
	if (SB_MDEN & 1)
	{
		for (const auto& pair : mapleDmaOut)
		{
			if (pair.first == 0)
			{
				asic_RaiseInterrupt(holly_MAPLE_OVERRUN);
				continue;
			}
			size_t size = pair.second.size() * sizeof(u32);
			u32 *p = (u32 *)GetMemPtr(pair.first, size);
			memcpy(p, pair.second.data(), size);
		}
		SB_MDST = 0;
		asic_RaiseInterrupt(holly_MAPLE_DMA);
	}
	else
	{
		INFO_LOG(MAPLE, "WARNING: MAPLE DMA ABORT");
		SB_MDST = 0; //I really wonder what this means, can the DMA be continued ?
	}
	mapleDmaOut.clear();

	return 0;
}

static void maple_SB_MDAPRO_Write(u32 addr, u32 data)
{
	if ((data >> 16) == 0x6155)
		SB_MDAPRO = data & 0x00007f7f;
}

//Init registers :)
void maple_Init()
{
	hollyRegs.setWriteHandler<SB_MDST_addr>(maple_SB_MDST_Write);
	hollyRegs.setWriteHandler<SB_MDEN_addr>(maple_SB_MDEN_Write);
	hollyRegs.setWriteHandler<SB_MSHTCL_addr>(maple_SB_MSHTCL_Write);
	hollyRegs.setWriteOnly<SB_MDAPRO_addr>(maple_SB_MDAPRO_Write);
#ifdef STRICT_MODE
	hollyRegs.setWriteHandler<SB_MDSTAR_addr>(maple_SB_MDSTAR_Write);
#endif

	maple_schid = sh4_sched_register(0, maple_schd);
}

static u64 reconnect_time;

void maple_Reset(bool hard)
{
	maple_ddt_pending_reset = false;
	SB_MDTSEL = 0;
	SB_MDEN   = 0;
	SB_MDST   = 0;
	SB_MSYS   = 0x3A980000;
	SB_MSHTCL = 0;
	SB_MDAPRO = 0x00007F00;
	SB_MMSEL  = 1;
	mapleDmaOut.clear();
	reconnect_time = 0;
}

void maple_Term()
{
	mcfg_DestroyDevices();
	sh4_sched_unregister(maple_schid);
	maple_schid = -1;
}

void maple_ReconnectDevices()
{
	mcfg_DestroyDevices();
	reconnect_time = sh4_sched_now64() + SH4_MAIN_CLOCK / 10;
}

void maple_ReconnectDevice(int bus, int port)
{
	if (port == 5)
	{
		// main device
		for (int i = 0; i <= 5; i++)
			MapleDevices[bus][i].reset();
	}
	else {
		// sub device
		MapleDevices[bus][port].reset();
	}
	reconnect_time = sh4_sched_now64() + SH4_MAIN_CLOCK / 10;
}

static void maple_handle_reconnect()
{
	if (reconnect_time != 0 && reconnect_time <= sh4_sched_now64())
	{
		reconnect_time = 0;
		mcfg_CreateDevices();
	}
}
