# Using an emulator as a lab bench: Flycast instrumentation for a Naomi → Dreamcast port

This fork of [Flycast](https://github.com/flyinghead/flycast) is not a better
emulator. It is a **measurement instrument** — the one that was used to port
the Sega Naomi arcade game **Cleopatra Fortune Plus** to the Sega Dreamcast by
*static binary conversion*: no source code, no devkit, just the original cart
dump, a disassembler, and this instrumented emulator. The same approach the
community used for the Atomiswave→Dreamcast ports.

You don't need to know anything about that project to use this fork. This
document explains, from scratch, what each probe measures, why it exists, and
what it actually found — because most of the *techniques* here (streaming
maps, guest-PC attribution, hang taxonomies, data watchpoints on strings,
byte-exact I/O capture) transfer directly to any similar conversion or to
emulator-assisted reverse engineering in general.

- **Base:** upstream `master` at `4126f1464`. The full change is
  `git diff 4126f1464..HEAD` — every hunk carries an in-place comment.
- **License:** GPL-2.0, same as upstream (see `LICENSE`).
- Nothing here is upstreamable and none of it is meant to be.

---

## 1. The problem: same silicon, different machine

The Naomi is almost a Dreamcast. Same Hitachi SH4 CPU, same PowerVR CLX2 GPU,
same AICA sound chip. That's what makes a static conversion thinkable at all:
the game's machine code runs unmodified on the target CPU. Everything that
breaks lives in the differences:

| | Naomi (arcade) | Dreamcast (console) |
|---|---|---|
| Main RAM | 32 MB | 16 MB |
| Video RAM | 16 MB | 8 MB |
| Sound RAM | 8 MB | 2 MB |
| Game storage | ROM cart (this game: 109 MB), memory-mapped, read by **G1 DMA** on demand | GD-ROM disc, block reads |
| Input | Arcade **JVS** bus (sticks/buttons/coins) behind the **MIE** — a Z80-based I/O controller the game talks to over Maple bus command `0x86` | Maple bus controllers |
| Settings storage | EEPROM on the cart board, accessed through BIOS routines | Flash / VMU |
| Video out | 31 kHz arcade monitor | 15 kHz TV / VGA, cable auto-detected via SH4 GPIO pins |
| BIOS | Naomi BIOS, which games may call back into | Dreamcast BIOS, completely different entry points |

A conversion has to answer, for each row: *does this game actually depend on
that, how much, and from where in its code?* Guessing is fatal — you'd patch
the wrong function or reserve the wrong memory. So instead of guessing, we
made the emulator answer every question with logs. That is this fork.

## 2. Method: three campaigns, two log channels

The instrumentation was built up across three distinct campaigns, and the
probes only make sense in that order:

1. **Observe the original.** Run the untouched Naomi ROM under Flycast's
   Naomi emulation and log *what the game uses*: which cart regions it
   streams, how much RAM it really touches, what it does with inputs, EEPROM,
   serial. Output: a requirements list for the port.
2. **Attribute to code.** Re-run with the SH4 **interpreter** (not the
   dynarec) and stamp every interesting hardware event with the **guest
   program counter** that caused it. Output: the exact functions in the game
   binary to patch — fed to Ghidra for static analysis.
3. **Debug the converted build.** The game, rebuilt as a Dreamcast disc image
   (a loader, a small resident runtime called the *shim*, and a patch table),
   now runs under Flycast's *Dreamcast* emulation — and fails in new and
   interesting ways. A third wave of probes dissects boot hangs and error
   screens.

Two log channels:

- **`cartlog(...)`** — a deliberately tiny new logger
  (`core/hw/naomi/cartlog.{cpp,h}`, ~20 lines). Appends printf-style lines to
  the file named by the **`FLYCAST_CARTLOG`** environment variable (default
  `flycast-cartlog.txt` in the working directory), `fflush`ed per line so a
  hung or killed emulator loses nothing — essential when the very thing you
  study is a hang. Every line starts with an uppercase tag (`CARTDMA`,
  `WATERMARK`, …) so a capture is one `grep` away from an answer. Not gated
  by any flag: this build always writes it.
- **`NOTICE_LOG(..., "CLEO-...")`** — low-volume probes woven into Flycast's
  normal logging, tagged `CLEO-` for grepping out of the standard log.

In-code comments reference the port project's internal plan as
`Phase N (Task M)`. Decoder: **Phase 2** = campaign 1 above, **Phase 3** =
campaign 2, **Phase 4** = campaign 3; Task 4 = shim I/O work, Task 13 =
boot-hang dissection, Task 14c = the I/O-board error screen, Task 18 =
headless screenshots.

---

## 3. Campaign 1 — what does the game actually use?

### 3.1 The cart-streaming map: `CARTDMA`, `CARTPIO`

**Question.** A Naomi game doesn't load once — the 109 MB cart is a
memory-mapped device the game streams from continuously via G1 DMA. On a
Dreamcast there is no cart; every one of those reads must be re-issued as a
GD-ROM disc read by the port's runtime. So: *which byte ranges does the game
ever ask for, and where does it put them?*

**Probe.** `Naomi_DmaStart()` (`core/hw/naomi/naomi.cpp`) fires on every
cart→RAM DMA the game programs:

```
CARTDMA src=%08x dest=%08x len=%x     cart byte offset, phys RAM dest, length
CARTPIO offset=%08x                   rare non-DMA reads via the PIO data port
```

The cart-side source offset wasn't exposed by the cartridge interface, so the
diff adds a virtual `NaomiCartridge::GetDmaSrcOffset()` (`naomi_cart.h`) that
reports the current `DmaOffset` (cart types that don't track one report 0).

**What it found.** Across an attract loop, an overnight demo run, and a
hands-on play-through to game over: **388 unique DMA requests**, spanning
cart offsets 8 MB…96 MB, every destination inside main RAM, every length
32-byte aligned — and exactly **one** PIO seek. The play pass added only 5
requests over what demo mode already exercised, so attract/demo coverage was
nearly complete. The top ~12 MB of the cart was never read by anything. That
list of 388 `(offset, length, dest)` triples *is* the port's disc layout
specification.

### 3.2 Does it even fit? `WATERMARK`

**Question.** The Dreamcast has half the main RAM, half the VRAM, a quarter
of the sound RAM. Does the game actually *use* more than the Dreamcast has?

**Probe.** Every 64th cart DMA, scan main RAM, VRAM and audio RAM backwards
for the highest non-zero byte:

```
WATERMARK region=main|vram|aram used=%x size=%x
```

A content scan is crude — stale non-zero garbage high in a region inflates
it — but that makes it a *conservative upper bound*, which is the correct
direction to be wrong in for a "will it fit" decision.

**What it found — including a lesson.** Main-RAM asset placement peaked at
11.2 MB (fits in 16 MB), VRAM at 9.2 MB (~1 MB over the DC's 8 MB — flagged
for texture cuts), sound RAM pegged at exactly 8 MB (the scan artifact case:
a stale byte at the very top). The instructive one: the main-RAM scan showed
a byte just under the **32 MB** line, which looked like the game keeping its
stack at top-of-RAM — a port-killer if true. Campaign 2's actual SP logging
(§4.1) later proved the stack lives at `0x8c00exxx`, ~60 KB above RAM base:
the 32 MB hit was stale data. *Moral: a cheap conservative probe buys you the
question; only a precise probe buys you the answer.*

**The precise probe: `ARAMPROFILE`.** The sound-RAM peg is the moral in
miniature — a content scan literally cannot tell a stale/BIOS byte from a real
game write, so "8 MB" meant nothing. The fix is to measure *writes*, not
content. At the first cart DMA (game handoff — cart DMAs target *main* RAM, so
sound reaches ARAM only later via G2/AICA DMA and nothing is lost) zero ARAM
once; afterward every non-zero byte is a genuine game/AICA sound write. Then
report the true high-water plus a 256 KB-bucket histogram (so a lone stray write
is distinguishable from dense use):

```
ARAMHANDOFF zeroed size=%x
ARAMPROFILE high=%x nz=%x nz_below2m=%x nz_above2m=%x size=%x
ARAMHIST <one non-zero-byte count per 256 KB bucket; bucket 8+ = past 2 MB>
```

**What it found.** The game writes only ARAM `0x0-0x1fffff` — **exactly 2 MB,
zero bytes above** — loaded once at boot as a fixed bank (histogram buckets 8-31,
the 2-8 MB range, stay all-zero). Stable across attract, demo, and a hands-on
gameplay round through drops/clears/combos/stage changes. The game already
targets a ≤2 MB sound config, so it fits the Dreamcast's 2 MB ARAM natively — no
sample cuts. The conservative scan asked "does it fit?"; the write-truth probe
answered "yes, exactly."

**The same probe, aimed at VRAM: `VRAMPROFILE`.** That left the 9.2 MB VRAM
flag — same disease, same cure, one new wrinkle. Zero VRAM at the same handoff
point (texture uploads can't precede the game's first asset fetch; the only
casualty is the BIOS boot screen) and profile genuine writes against the DC's
8 MB line. The wrinkle is a Flycast blind spot: the TA parses display lists
into host-side structures and rendering happens on the host GPU, so ISP/OL
buffers and framebuffer pixels never appear as vram-array *content*, even
though they occupy real VRAM on hardware. `VRAMREGS` covers those regions by
*layout* instead — the TA base/limit and framebuffer start registers — so the
real footprint is max(content high-water, TA limits, FB extents):

```
VRAMHANDOFF zeroed size=%x
VRAMPROFILE high=%x nz=%x nz_below8m=%x nz_above8m=%x size=%x
VRAMHIST <one non-zero-byte count per 256 KB bucket; bucket 32+ = past 8 MB>
VRAMREGS isp_base=%x isp_limit=%x ol_base=%x ol_limit=%x fb_w_sof1=%x fb_w_sof2=%x fb_r_sof1=%x
```

**What it found.** The 9.2 MB was never game data: the Naomi BIOS parks its
framebuffers at `0x800000`/`0xc00000`, and the boot screen drawn there sat in
VRAM as stale bytes the content scan dutifully counted. The game's own writes
peak at 7.8 MB with **zero bytes at or above the 8 MB line** in every snapshot
(attract and a hands-on play round agree), and its own layout double-buffers
two 4 MB banks — TA lists at `0x0`/`0x400000`, framebuffers at
`0x0b2000`/`0x4b2000` — everything below `0x800000`. (Address-space footnote:
the content scan indexes the 64-bit/texture path while the registers hold
32-bit-path addresses; `pvr_map32()` in `core/hw/pvr/pvr_mem.cpp` preserves
bit 23, so "below 8 MB" in either space means the lower physical 8 MB.) Both
fit flags the crude scan raised were stale-byte artifacts; the write-truth
probe closed both.

### 3.3 Deleting a problem with one grep: `SERIALPOKE`

**Question.** Naomi boards have a serial/network interface (`NAOMI_COMM_*`
registers). If the game touches it, the port needs a stub for it.

**Probe.** Log every write into that register range
(`WriteMem_naomi`, `core/hw/naomi/naomi.cpp`):

```
SERIALPOKE addr=%08x data=%08x
```

**What it found.** Zero lines, across every capture. Requirement deleted.
Cheapest probe in the fork, best return on investment.

### 3.4 The input map: `JVSREPORT`

**Question.** Arcade sticks and buttons arrive as a 16-bit JVS word per
player; the Dreamcast pad delivers a completely different structure. To write
the input shim you need the exact bit layout *as this game reads it* — and
the polarity.

**Probe.** In the JVS digital-input handler (`core/hw/maple/maple_jvs.cpp`),
log Player 1's word on every poll:

```
JVSREPORT buttons=%04x
```

**What it found.** Idle is `0x0000` in 4794 of 4794 idle reports → the word
is **active-high** (which contradicted the initial assumption — the captures
settled it). Then a supervised session pressing one control at a time
produced a clean single-bit word per control (`8000` Start, `2000`/`1000`/
`0800`/`0400` directions, `0200`/`0100` the two buttons), confirming the
standard JVS layout bit by bit. That table is the pad-mapping contract for
the port's input shim.

---

## 4. Campaign 2 — which code does it?

Knowing *that* the game streams the cart is not enough; the port must patch
the **function** that does it. These probes stamp hardware events with the
guest PC. That only works reliably on the interpreter — see
[§8](#8-interpreter-vs-dynarec-which-probes-need-which) for why the dynarec
lies to you here.

### 4.1 `CARTDMAPC` — finding the cart-read function

At every cart-DMA kick, additionally log where the guest was:

```
CARTDMAPC pc=%08x sp=%08x
```

The PC pins the game's cart-read routine (one small function, patched by the
port to call the shim's disc reader instead). The SP, logged for free,
resolved the stack question from §3.2: real SP range `0x8c00e6e8`–
`0x8c00ef28` across full captures — nowhere near the 32 MB scare.

### 4.2 `MAPLEPC` — finding the input and EEPROM functions

On Naomi, the game talks to the MIE (the Z80-based I/O controller) through
Maple command `0x86` transactions with a subcommand byte: input polls,
EEPROM reads, EEPROM writes all go through the same funnel. Log each one with
its guest PC (`MIEImpl::handle_86_subcommand`, `core/hw/maple/maple_jvs.cpp`):

```
MAPLEPC cmd=86 sub=%02x pc=%08x
```

Subcommand decoding for this game: `sub=15`/`33` input polls, `01`/`03`
EEPROM read/write. Counting occurrences also mattered: the *steady-state*
input poll turned out to be a different subcommand (`0x33`, tens of
thousands of hits) than the boot-time one (`0x15`) — so the port had to shim
**two** call sites, which a single-sample measurement would have missed.

### 4.3 `BIOSEXEC` — does the game call back into the BIOS?

**Question.** After the Naomi BIOS hands control to the game, does the game
ever jump back *into* BIOS code? Any such path is invisible to static
analysis of the game binary alone — and every BIOS entry point used is
something the port must replace, because the Dreamcast BIOS has none of them.

**Probe.** In the interpreter's instruction-fetch path
(`core/hw/sh4/interpr/sh4_interpreter.cpp`): once the game's entry point
(`0x8c04ae2c` for this binary) has been fetched, flag any subsequent fetch
inside BIOS ROM (physical `< 0x200000`), deduplicated per PC:

```
BIOSEXEC pc=%08x
```

**What it found.** The game *does* re-enter the BIOS: it thunks into a
settings/EEPROM library the Naomi BIOS keeps at ROM offset `0x60000`. That
library bit-bangs the cart-board EEPROM through hardware registers that don't
exist on a Dreamcast — on real DC hardware it spins forever on a G1 status
poll. This single log line predicted what later became the hardest real-
hardware bug of the port. If you instrument only one thing on a conversion,
instrument this.

---

## 5. Campaign 3 — debugging the converted build

From here on, the game is no longer running as a Naomi ROM. It has been
rebuilt as a Dreamcast disc: a loader, the patched game, and a small
resident runtime — the **shim** — parked in the last 256 KB of Dreamcast RAM
(physical `0x0cfc0000–0x0cffffff`). The patched game no longer programs the
G1 cart-DMA registers; it writes the same programming sequence into a fake
"register mirror" block inside the shim's RAM (physical
`0x0cfc8800–0x0cfc9000`, with the DMA "go" trigger mirrored at offset
`+0x418`, i.e. `0x0cfc8c18`), and the shim services it with GD-ROM reads.
Three probe families exist to watch exactly that machinery.

### 5.1 `SHIMWATCH` — is the shim's home actually free?

**Question.** The shim occupies RAM the original game supposedly never
touches. *Supposedly* — prove it.

**The trap.** The obvious probe is a write hook on that address range. It
would silently miss most writes: Flycast's ARM64 **dynarec fast path** stores
register-indirect writes straight into host-mapped RAM, bypassing every
C-level write function (`core/rec-ARM64/rec_arm64.cpp`,
`GenWriteMemoryFast`/`GenWriteMemoryImmediate`). Your hook sees a quiet
region; the game scribbles all over it. This class of bug — *instrumenting
the emulator's slow path while the fast path does the work* — is worth
internalizing before trusting any emulator-based measurement.

**Probe.** Don't hook writes; scan **content**. At the same 64-DMA cadence as
`WATERMARK`, scan the range for any non-zero byte; one-shot report:

```
SHIMWATCH addr=%08x
```

A write that lands and is zeroed again between samples would be missed —
accepted trade-off, same as any sampling scan. **Result:** never fired
during Naomi-mode runs; the region is genuinely free.

### 5.2 `MIRRORWR` — watching the patched game talk to the shim

Log every guest store into the mirror block, with the storing PC
(`writet()`, `core/hw/mem/addrspace.cpp`):

```
MIRRORWR pc=%08x off=%03x val=%08x
```

This shows the patched game's full DMA programming conversation — cart
offset, destination, length, then `1` to the `+0x418` trigger — and *who*
wrote each word, which is how you notice a patch you forgot, an alias you
didn't expect (the game reaches the block through its uncached `0xacfcxxxx`
mapping), or a write arriving out of order.

### 5.3 Byte-exact I/O forgery: `MIERESP`, `CLEO-MIE`

**Question.** The DC build has no MIE, but the game still runs its full MIE
conversation at boot — reset, GetID, even uploading firmware to the MIE's
Z80 — and then polls it for input forever. The shim must impersonate the MIE
convincingly. Impersonation needs a transcript.

**Probes.** Two capture points, chosen so the logged bytes are exactly what
the *game* sees, not what the emulator computed internally:

- `MIERESP sub=%02x addr=%08x data=<128 hex chars>` — in `maple_DoDma()`
  (`core/hw/maple/maple_if.cpp`), for JVS `0x86` transactions: a fixed
  64-byte dump of the reply, taken *after* the device handler ran **and
  after the byte-order fixup**, i.e. byte-identical to what lands in guest
  RAM at the destination (`addr=`) from the Maple descriptor. (The reply
  buffer is zeroed beforehand so the fixed-size dump can't leak uninitialized
  host stack into the log.)
- `CLEO-MIE cmd=%02x hdr_in=%08x replylen=%d reply=<hex>` — in
  `BaseMIE::RawDma()` (`core/hw/maple/maple_jvs.cpp`), same idea for every
  **non-`0x86`** MIE command: the boot-time init ladder, byte-exact.

Together these gave the shim its reference corpus: for each request the game
makes, the exact reply a real MIE produces. On real hardware the game runs
this ladder even though Flycast's high-level boot skips parts of it — the
transcript is what made the shim's replies survive contact with a real
Dreamcast.

---

## 6. Interlude — four ways an emulated CPU can look hung

The converted build's first boots on the Dreamcast side died as a black
screen. "It hangs" is not a bug report; the Task-13 kit turns it into one.
The insight worth stealing: **a guest can look frozen in at least four
mechanically different ways, and each starves a different naive detector.**
All lines include `gdst_mirror=`, the shim's mirrored DMA trigger word — the
single most informative variable for *this* port's hangs ("is the game
waiting on a disc read the shim never completed?").

| Failure mode | Why the naive detector misses it | Probe |
|---|---|---|
| **Busy spin** — a `bf`-to-self poll loop | A periodic "print the PC" in the outer scheduler loop works here — but only here | `HANG pc=… gdst_mirror=…` (`sh4_interpreter.cpp`): >50 M consecutive fetches inside one <32-byte PC window. The threshold is far above any real init/delay loop (largest observed ~1 M iterations) yet a true spin crosses it in under a second. One-shot. |
| **Paced idle** — guest parked on `SLEEP` | Fetch counters barely advance (the emulator spends ~a real second *inside each `SLEEP`*, servicing the scheduler), so spin detectors and fetch-based samplers starve | `SLEEPWAIT pc=… pr=… gdst_mirror=…` (`sh4_opcodes.cpp`): logged at the `SLEEP` opcode itself, ~1/sec; `pr` names who called the wait routine. |
| **Exception storm** — a fault taken every instruction (e.g. before the guest installs its VBR handlers) | Control short-circuits from the fault straight back to the dispatch loop; the fetch-path sampler never runs | `EXC epc=… evn=… newpc=… vbr=…` (`Run()`'s exception path): faulting PC, SH4 event code (`0x180` illegal instruction, `0x0e0`/`0x100` address errors, …), vector taken. ~1/sec. |
| **Stuck inside one MMIO access** — the guest executed a load/store whose *emulator-side handler* loops, so the guest never even finishes one instruction | From the guest's perspective time has stopped; no guest-side detector can fire at all | `HWR`/`HWW pc=… addr=… val=…` (`addrspace.cpp`): every MMIO access from the game's own code region, consecutive duplicates collapsed. When everything else freezes, **the last `HWR`/`HWW` line names the register whose handler you're stuck in.** |

Plus a heartbeat that survives three of the four:
`PCSAMPLE pc=%08x gdst_mirror=%08x` — a **wall-clock** 1 Hz sample placed in
the instruction-*fetch* path, not the outer loop, precisely because a
heavily real-time-paced inner loop may not reach the outer loop for seconds.

### 6.1 The culprit it caught: `MDODMA*`

The fourth failure mode was the real one. Flycast's `maple_DoDma()` runs
**synchronously inside the guest's `SB_MDST=1` store** and walks the Maple
descriptor list with an unbounded `while (!last)` — `last` being a bit in
each descriptor header. Feed it a malformed list whose end-bit never comes
(easy to do when you're forging Maple traffic from a hand-written shim) and
the emulator walks memory forever *inside one guest store instruction*:
guest frozen, host CPU pinned, every guest-side probe silent.

The probe brackets the walk and adds an escape hatch:

```
MDODMA enter mdstar=%08x hdr0=%08x mden=%d pc=%08x    entering the walk
MDODMA rawdma_call cmd=%02x reci=%02x bus=%d plen=%u  per frame, before the device handler
MDODMA rawdma_ret outlen=%x                            after it ("call" with no "ret" = handler hung)
MDODMA frame_done outlen=%x                            reply queued
MDODMA_RUNAWAY iters=%u addr=%08x hdr1=%08x            guard tripped
```

After 100 000 descriptors the guard logs `MDODMA_RUNAWAY`, clears `SB_MDST`
(so the guest's completion poll can exit) and abandons the walk — **a
deliberate behavior change** (upstream would loop forever), not a fix: it
exists so the run survives long enough to show where the guest goes next.

---

## 7. Case study — the "I/O BD IS NOT CONNECTED" screen

With boot unstuck, the game came up… onto an arcade error screen: it had
probed for its JVS I/O board, and the shim's MIE impersonation hadn't
convinced it. The question became: *which check, exactly, decides to show
this screen?* Two probes, and a third as a general tool — a nice showcase of
what you can do when the "debugger" is an emulator you can recompile:

- **Data watchpoint on the message itself.** The error strings sit at
  physical `0x0c0ca6dc–0x0c0ca740` with **no static cross-reference** — the
  game computes the address as resource-base + offset, so Ghidra shows
  nothing pointing at them. So trap the *read*: in `readt()`, log any access
  to those bytes with reader PC and PR, deduped per address:

  ```
  STRWATCH pa=%08x pc=%08x pr=%08x
  ```

  The reading PC is the text renderer; its PR (return address) starts the
  walk back up the call chain toward the decision.

- **Software breakpoints without a debugger.** Once static analysis narrowed
  the decision to the game's scene loop, four PCs inside it
  (`0x0c04b08a/90/1fa/200`) were turned into logging breakpoints by a PC
  compare in the interpreter's fetch path. Each hit snapshots the scene
  object (`*0x8c0c4510`), two of its vtable-method results, the game's
  I/O-enumeration flags, and `r0` — the value the gate method just returned:

  ```
  IOCHK pc=… r0=… obj=… m10=… m7c=… conn=… specs=… mir=…
  ```

  Zero-setup, survives reboots, costs one compare per fetched instruction —
  for interpreter-mode work this is often *better* than a real debugger.

- **Plain data watchpoints.** Same trick for two game variables
  (`0x0c0e842c`, `0x0c0e6298`) whose writers needed identifying:

  ```
  CLEO-WATCH [%08x] = %08x pc=%08x pr=%08x
  ```

---

## 8. Hardware-difference tripwires: the `CLEO-*` probes

Low-volume change-loggers in Flycast's standard log, each guarding one known
Naomi↔Dreamcast difference. They log only on *change*, with the writing PC,
so they read like a story rather than a firehose:

| Tag | Where | Guards against |
|---|---|---|
| `CLEO-SPG` | `core/hw/pvr/pvr_regs.cpp` | Video-timing programming: any change to `SPG_LOAD`, `FB_R_CTRL` (incl. the pixel-clock divider bit), `VO_CONTROL`, `SPG_HBLANK/VBLANK/WIDTH`, `VO_STARTX/Y`, and the scanout geometry `FB_R_SIZE` / `FB_R_SOF1` / `FB_R_SOF2` (added for the composite-loadbar bug: the game's TV mode scans only FB lines 0..236, which `FB_R_SIZE` alone reveals). A Naomi game programs a 31 kHz arcade monitor; if nobody re-programs those registers for a TV, this log shows you exactly who set what, from where. |
| `CLEO-VRAMDUMP` | `core/hw/pvr/pvr_regs.cpp` | Raw-VRAM snapshots for CPU framebuffer paints the render path never shows (loadbar/HUD/splash). `FLYCAST_VRAMDUMP=<prefix>` writes `<prefix>-NN.bin` every 512 `FB_R_SOF*` writes (max 40) and `<prefix>-shim-NN.bin` on every `VO_CONTROL` write whose PC is in the shim (`0x8cfcxxxx` — the unblank right after a paint; max 20). Dumps are the **64-bit-view** VRAM array; to inspect pixels addressed through the 32-bit area (SOF bases, shim paints), apply `pvr_map32()`'s bank swizzle (`pvr_mem.cpp`) to each byte offset. |
| `CLEO-GPIO` | `core/hw/sh4/modules/bsc.cpp` | SH4 GPIO port writes (`PDTRA`, and `PCTRA` — which gets explicit, semantically identical read/write handlers so it can log). On a Dreamcast these pins are the **video-cable auto-detect**; on a Naomi they mean something else. A conversion that lets Naomi-era GPIO writes through can convince a Dreamcast it has the wrong cable. |
| `CLEO-CCR` | `core/hw/sh4/modules/ccn.cpp` | Cache-control writes (masking the transient invalidate bits): spots the game reconfiguring the SH4 cache/OC-RAM behind your back. |
| `CLEO-ARMRST` | `core/hw/aica/aica_if.cpp` | AICA ARM reset release, plus the first two words of sound RAM at that instant — i.e. *was a sound driver actually uploaded before the ARM was let loose?* All zeros = the ARM is about to execute garbage. |

---

## 9. Seeing the screen without asking the OS: `FLYCAST_SHOT`

Long unattended captures (and AI-agent-driven debugging sessions) need to
answer "what is on screen right now?" without a human watching — and on
macOS, without triggering the screen-recording permission (TCC) machinery.
So the emulator photographs *itself*: `gui_dumpFramebuffer()`
(`core/ui/gui.cpp`, called after each rendered frame from
`core/ui/mainui.cpp`) reuses Flycast's own screenshot readback
(`getScreenshot()` → renderer `GetLastFrame()`) and writes a 640×480 RGB PNG.

- `FLYCAST_SHOT=/abs/path/shot.png` enables it (zero-cost when unset).
- The file is rewritten every `FLYCAST_SHOT_EVERY` frames (default 60), and
  **on `SIGUSR1`** — `kill -USR1 <pid>` — for an on-demand grab. The signal
  is the reliable way to capture a *specific* screen; the periodic dump loves
  to sample a transition/black frame.
- Read with copy-then-open; the write is a single `fwrite` but not atomic.

---

## 10. Honesty section: what changes emulator behavior

Everything in this fork is pure logging, **except**:

1. **The `maple_DoDma` runaway guard** (§6.1): aborts a >100 000-descriptor
   walk, clears `SB_MDST`, logs. Upstream loops forever; this build escapes.
   A diagnostic escape hatch — if you're validating final images, be aware
   it can mask a genuinely malformed descriptor list.
2. **Reply-buffer zeroing** in `maple_DoDma`: guest-visible bytes unchanged
   (only `outlen` bytes were ever copied to the guest); it exists so the
   fixed-size `MIERESP` dump can't read uninitialized host stack.
3. **`BSC_PCTRA` handler swap**: auto read/write register → explicit
   handlers with identical semantics, purely to gain the log line.
4. **`ARMRST` log level** raised INFO → NOTICE.
5. **macOS build fixes**, orthogonal to instrumentation: top-level
   `CMakeLists.txt` adds `enable_language(OBJC)` (the SDL2 subproject
   compiles `.m` files but never enables the language — breaks under
   command-line tools + CMake 3.31) and only bundles MoltenVK when Vulkan is
   actually enabled and an SDK exists. A third fix lives in
   `patches/flycast-syphon-build-fix.diff` because it targets the
   `core/deps/Syphon` **submodule** (a fork can't carry submodule commits):
   it makes Syphon's ObjC prefix header `PRIVATE` so it stops poisoning
   Flycast's Objective-C++ sources.

## 11. What's game-specific (re-derive these for your game)

The techniques are general; these constants are baked in for the Cleopatra
Fortune Plus binary and this port's memory layout:

| Constant | Used by | Meaning |
|---|---|---|
| `0x8c04ae2c` | `BIOSEXEC` arming | This game's entry point (BIOS→game handoff). |
| phys `0x0c020000–0x0c200000` | `HWR`/`HWW` PC filter | The game's own code region (its boot image loads at `0x8c020000`); filters out loader/BIOS noise. Matching is on the *physical* PC so cached (`0x8c…`) and uncached (`0xac…`) aliases both count. |
| phys `0x0cfc0000–0x0cffffff` | `SHIMWATCH` | Port-defined shim residence. |
| phys `0x0cfc8800–0x0cfc9000` (+`0x418` trigger) | `MIRRORWR`, every `gdst_mirror=` field | Port-defined G1 register-mirror block. |
| phys `0x0c0ca6dc–0x0c0ca740` | `STRWATCH` | The error-string block being hunted in §7. |
| `0x0c04b08a/90/1fa/200`, `0x8c0c4510`, `0x8c1c9774`, `0x8c0d541c`, `0x8c127b0c` | `IOCHK` | Scene-loop breakpoint PCs, scene-object pointer, I/O-enumeration flags. |
| phys `0x0c0e842c`, `0x0c0e6298` | `CLEO-WATCH` | Two watched game variables. |

One implementation detail you'll want when adapting: probes in memory-access
handlers log `pc - 2`, because the interpreter's `ReadNexOp` advances
`ctx->pc` to the *next* instruction before executing the current one — the
raw `pc` at handler time points one instruction past the access.

## 12. Interpreter vs dynarec: which probes need which

Rule of thumb: **anything that reports a `pc=` needs the interpreter;
anything that hooks a device fires regardless.**

- The ARM64 dynarec's fast path performs RAM loads/stores inline in
  generated code, bypassing `readt`/`writet` — so `MIRRORWR`, `STRWATCH`,
  `CLEO-WATCH`, `HWR`/`HWW` go blind (see the §5.1 trap). And nothing
  outside the interpreter fetches instructions through `ReadNexOp`, so
  `BIOSEXEC`, `HANG`, `PCSAMPLE`, `IOCHK` (and `SLEEPWAIT`, `EXC`) never
  fire. Even in device probes that *do* fire, the `pc=` field is only
  instruction-exact under the interpreter.
- Device/MMIO probes work under either engine: `CARTDMA`, `CARTPIO`,
  `WATERMARK`, `SHIMWATCH` (content scan — that's the point), `ARAMHANDOFF` /
  `ARAMPROFILE` / `ARAMHIST` and `VRAMHANDOFF` / `VRAMPROFILE` / `VRAMHIST` /
  `VRAMREGS` (fire from the cart-DMA handler, same as `WATERMARK`),
  `SERIALPOKE`, `JVSREPORT`, `MIERESP`, `MDODMA*`, and all `CLEO-*`.

Enable the interpreter with `Dynarec.Enabled=no` under `[config]` in
`emu.cfg`, or select the Interpreter in the GUI's CPU settings. Budget ~10×
slowdown; captures here typically ran minutes, not hours.

## 13. Building (macOS notes) and a capture cookbook

General builds: follow upstream. This fork was developed on macOS/arm64,
which additionally needed:

- **CMake 3.31.x** (Kitware binary — Homebrew's CMake 4.x fails at generate
  time on this base).
- Full Xcode: `export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer`
  (Objective-C ABI detection shells out to `xcodebuild`; CommandLineTools
  alone fails).
- Submodules, then the Syphon patch **inside the submodule**:

```sh
git submodule update --init --recursive
git -C core/deps/Syphon apply "$PWD/patches/flycast-syphon-build-fix.diff"
export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
ZLIB_TBD="$DEVELOPER_DIR/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk/usr/lib/libz.tbd"
cmake -B build -DCMAKE_BUILD_TYPE=Release \
      -DUSE_BREAKPAD=OFF -DUSE_VULKAN=OFF \
      -DCMAKE_OSX_ARCHITECTURES=arm64 -DZLIB_LIBRARY="$ZLIB_TBD"
cmake --build build -j"$(sysctl -n hw.ncpu)"
```

Two macOS gotchas that each cost a debugging session: launch unattended runs
with `-config config:rend.vsync=no` (otherwise the emu thread can deadlock
once the window loses focus), and
`defaults write com.flyinghead.Flycast ApplePersistenceIgnoreState -bool YES`
(after a killed instance, macOS's window-restore state silently blocks the
*next* launch — process alive at ~0 % CPU, guest never boots).

A typical session:

```sh
FLYCAST_CARTLOG=/tmp/capture.log \
FLYCAST_SHOT=/tmp/shot.png FLYCAST_SHOT_EVERY=60 \
  ./build/flycast -config config:rend.vsync=no /path/to/game &
# ... let it boot / attract / play ...
kill -USR1 %1                                   # screenshot on demand
grep '^CARTDMA '  /tmp/capture.log | sort -u    # the streaming map
grep '^WATERMARK' /tmp/capture.log | tail -3    # RAM high-water marks
grep '^JVSREPORT' /tmp/capture.log | uniq -c    # input words seen
```

## 14. Quick tag reference

| Tag | Channel | One-liner |
|---|---|---|
| `CARTDMA src dest len` | cartlog | Cart→RAM DMA request (the streaming map). |
| `CARTPIO offset` | cartlog | Cart PIO seek (non-DMA reads). |
| `WATERMARK region used size` | cartlog | Highest non-zero byte in main/VRAM/ARAM (upper bound). |
| `SERIALPOKE addr data` | cartlog | Write to Naomi serial/network registers. |
| `JVSREPORT buttons` | cartlog | P1 JVS digital word, active-high, per poll. |
| `CARTDMAPC pc sp` | cartlog | Guest PC/SP at cart-DMA kick (interpreter). |
| `MAPLEPC cmd=86 sub pc` | cartlog | Guest PC per MIE subcommand: input/EEPROM sites (interpreter). |
| `BIOSEXEC pc` | cartlog | Post-handoff execution inside BIOS ROM (interpreter). |
| `SHIMWATCH addr` | cartlog | Non-zero byte found in the shim's home range (content scan). |
| `MIRRORWR pc off val` | cartlog | Guest store into the shim's G1 mirror block (interpreter). |
| `MIERESP sub addr data` | cartlog | Byte-exact 64-byte MIE reply + guest destination (cmd `0x86`). |
| `CLEO-MIE cmd hdr_in replylen reply` | NOTICE_LOG | Byte-exact MIE reply, non-`0x86` commands. |
| `PCSAMPLE pc gdst_mirror` | cartlog | 1 Hz wall-clock PC heartbeat (interpreter). |
| `HANG pc gdst_mirror` | cartlog | Busy-spin detector (>50 M fetches in a <32 B window). |
| `SLEEPWAIT pc pr gdst_mirror` | cartlog | Guest parked on `SLEEP`, ~1/sec. |
| `EXC epc evn newpc vbr` | cartlog | Exception-storm sampler, ~1/sec. |
| `HWR`/`HWW pc addr val` | cartlog | MMIO access from game code, dupes collapsed (interpreter). |
| `MDODMA …` / `MDODMA_RUNAWAY …` | cartlog | Maple DMA walk bracketing + runaway guard. |
| `STRWATCH pa pc pr` | cartlog | Read of the watched string bytes (data watchpoint, interpreter). |
| `IOCHK pc r0 obj …` | cartlog | Logging breakpoints in the scene loop (interpreter). |
| `CLEO-WATCH [addr] = val pc pr` | NOTICE_LOG | Data watchpoint on two game variables (interpreter). |
| `CLEO-SPG …` | NOTICE_LOG | Video-timing register changes, with writer PC. |
| `CLEO-GPIO …` | NOTICE_LOG | SH4 GPIO (`PDTRA`/`PCTRA`) changes, with writer PC. |
| `CLEO-CCR …` | NOTICE_LOG | Cache-control register changes. |
| `CLEO-ARMRST …` | NOTICE_LOG | AICA ARM reset release + first sound-RAM words. |
| `GDPIO fad secs type crc` | cartlog | CRC-32/IEEE of each PIO sector-batch refill (drive-truth, senkosp Phase 5). |
| `GDDMA fad secs type crc` | cartlog | CRC-32/IEEE of each DMA sector-batch fill (drive-truth, senkosp Phase 5). |
| `TEXERR idx code d98` | cartlog | senkosp's texture-error classifier cells (0x8c1a20a0/a8/98), sampled every 64th STARTRENDER; baseline-and-compare, emitted only on change (senkosp Phase 5 Task 5). |
| `TEXERRSAVE code slot path` / `TEXERRSAVE FAILED code reason` | cartlog | One-shot auto-savestate fired on the TEXERR code cell's 0->nonzero transition. Armed on the emu thread (`cartlog_texerr_tick()`), executed on the render thread (`cartlog_texerr_save_poll()`, called once per frame from `mainui_rend_frame()`) via `emu.stop(); dc_savestate(0); emu.start();` — `emu.stop()` joins the emu thread's own async result and self-join-deadlocks if called from that thread, so the save cannot happen where it's detected (senkosp Phase 5 Task 6). |
