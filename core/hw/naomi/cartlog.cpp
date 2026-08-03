// core/hw/naomi/cartlog.cpp
// Phase 2 instrumentation (Cleopatra Naomi->DC port). Not upstream.
#include "cartlog.h"
#include <cstdio>
#include <cstdlib>
#include <cstdarg>

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
