// core/hw/naomi/cartlog.cpp
// Phase 2 instrumentation (Cleopatra Naomi->DC port). Not upstream.
#include "cartlog.h"
#include <cstdio>
#include <cstdlib>
#include <cstdarg>

bool cartlog_enabled()
{
	static const bool on = getenv("FLYCAST_CARTLOG") != nullptr;
	return on;
}

void cartlog(const char *fmt, ...)
{
	static FILE *f = nullptr;
	if (f == nullptr)
	{
		const char *path = getenv("FLYCAST_CARTLOG");
		if (path == nullptr)
			return; // instrumentation off unless explicitly pointed at a file (no stray CWD logs)
		f = fopen(path, "w");
		if (f == nullptr)
			return; // ponytail: if the log file won't open, stay silent rather than crash the emu
	}
	va_list ap;
	va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	va_end(ap);
	fflush(f);
}

// round 13: last-N writes to the SDK transfer-queue slots, dumped at the
// transition C2D so the submit path's PCs are visible without flooding the
// log during the load screen (the same slots are reused every frame).
namespace {
struct RingNote { unsigned pa, val, pc, pr; int sz; };
RingNote ringnotes[96];
unsigned ringhead = 0, ringcount = 0;
}

void cartlog_ringnote(unsigned pa, unsigned val, unsigned pc, unsigned pr, int sz)
{
	ringnotes[ringhead] = { pa, val, pc, pr, sz };
	ringhead = (ringhead + 1) % 96;
	if (ringcount < 96)
		ringcount++;
}

void cartlog_ringdump(const char *why)
{
	cartlog("RINGDUMP %s n=%u\n", why, ringcount);
	unsigned start = (ringhead + 96 - ringcount) % 96;
	for (unsigned i = 0; i < ringcount; i++) {
		const RingNote &n = ringnotes[(start + i) % 96];
		cartlog("SLOTWR [%08x] sz=%d val=%08x pc=%08x pr=%08x\n",
				n.pa, n.sz, n.val, n.pc, n.pr);
	}
	ringcount = 0;
}
