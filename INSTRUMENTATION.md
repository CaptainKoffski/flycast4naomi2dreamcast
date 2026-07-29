# Flycast instrumentation for a Naomi → Dreamcast static conversion

This fork of [Flycast](https://github.com/flyinghead/flycast) carries the
instrumentation that was used to port the Sega Naomi game **Cleopatra Fortune
Plus** to the Sega Dreamcast by *static binary conversion* — no source code,
in the tradition of the community Atomiswave→Dreamcast ports. The emulator was
used as a **measurement instrument**: first to map what the Naomi game actually
uses (cart data, RAM, inputs, EEPROM, video timing), then to debug the
converted Dreamcast build (boot hangs, I/O-board checks, the port's runtime
shim) without any hardware debugger.

None of this is meant for upstream. It is shared so that anyone attempting a
similar Naomi/Atomiswave→DC conversion can reuse the probes — most of the
techniques are generic even where the constants are game-specific.

- **Base:** upstream `master` at `4126f1464` (developed and battle-tested at
  `f09d1f22`, which differs from this base only by a NetBSD CI change).
- **Full diff:** `git diff 4126f1464..HEAD` — every hunk is commented in place
  with a `Phase N (Task M)` marker explaining why it exists.
- **License:** GPL-2.0, same as upstream (see `LICENSE`).

## The "Phase / Task" vocabulary in the code comments

The in-code comments reference the port project's internal plan:

| Marker | Meaning |
|---|---|
| **Phase 2** | Instrumented analysis of the *original Naomi game* running under Flycast's Naomi emulation: cart-streaming map, RAM watermarks, input map, serial usage. |
| **Phase 3** | Reverse engineering: attributing hardware events to *guest code addresses* (interpreter-mode capture feeding Ghidra static analysis). |
| **Phase 4** | Conversion debugging: the game rebuilt as a Dreamcast GDI (loader + freestanding shim + patch table), now running under Flycast's *Dreamcast* emulation. |
| Task 4 | Shim development: input/EEPROM interception, MIE reply capture. |
| Task 13 | DC-mode boot-hang dissection. |
| Task 14c | Hunting the game's "I/O BD IS NOT CONNECTED" error-screen decision. |
| Task 18 | Headless framebuffer screenshots for unattended verification. |

## Architecture: two log channels

1. **`cartlog(...)`** — new tiny logger (`core/hw/naomi/cartlog.{cpp,h}`).
   Appends printf-style lines to the file named by the **`FLYCAST_CARTLOG`**
   environment variable (default: `flycast-cartlog.txt` in the working
   directory), flushed per line so a hung/killed emulator loses nothing.
   All high-volume structured probes write here, one greppable tag per line.
   Note: it is **not** gated by a flag — this build always produces the file.
2. **`NOTICE_LOG(... "CLEO-..." ...)`** — low-volume probes injected into
   Flycast's normal logging, tagged `CLEO-` so they can be grepped out of the
   standard log/console.

## Probe reference

### Phase 2 — what does the game actually use?

Run the original Naomi ROM under this build and the log answers: what must the
Dreamcast port provide?

| Tag | Where | Line format | Question it answers |
|---|---|---|---|
| `CARTDMA` | `core/hw/naomi/naomi.cpp` `Naomi_DmaStart()` | `CARTDMA src=%08x dest=%08x len=%x` | Every cart→RAM DMA: cart byte offset, physical RAM destination, length. Builds the **cart-streaming map** — which regions of the (much larger) cart ROM the game ever reads, so they can be repacked onto a GD-ROM and reissued as disc reads. `src` comes from a new virtual `NaomiCartridge::GetDmaSrcOffset()` (`naomi_cart.h`; returns 0 for cart types that don't track it). |
| `CARTPIO` | `core/hw/naomi/naomi_cart.cpp` `WriteMem` (ROM_OFFSETL) | `CARTPIO offset=%08x` | Cart reads that go through the PIO port instead of DMA (they'd be invisible to the DMA map). |
| `WATERMARK` | `naomi.cpp`, sampled every 64th cart DMA | `WATERMARK region=main\|vram\|aram used=%x size=%x` | Highest non-zero byte in main RAM / VRAM / audio RAM. Naomi has 32 MB main RAM, the Dreamcast 16 MB — does the game actually fit? (Backwards scan; stale data over-reports, i.e. a conservative upper bound.) |
| `SERIALPOKE` | `naomi.cpp` `WriteMem_naomi` | `SERIALPOKE addr=%08x data=%08x` | Writes to the `NAOMI_COMM_*` (network/serial board) registers — does the port need to stub a comm board? |
| `JVSREPORT` | `core/hw/maple/maple_jvs.cpp` JVS digital-input reply | `JVSREPORT buttons=%04x` | Player-1 JVS digital word on every input poll (active-high). Pressing each cabinet button while watching this line produced the **input map** for the DC pad shim. |

### Phase 3 — where in the game's code does it happen?

Same Naomi-mode capture, but attributing events to guest PCs so the functions
could be found in Ghidra and patched. **Interpreter mode required** (see
[caveat](#interpreter-vs-dynarec) below).

| Tag | Where | Line format | Question it answers |
|---|---|---|---|
| `CARTDMAPC` | `naomi.cpp` `Naomi_DmaStart()` | `CARTDMAPC pc=%08x sp=%08x` | Guest PC and stack pointer at the moment the game kicks a cart DMA → locates the game's **cart-read function** (the port patches it to read from GD-ROM instead) and pins where the stack lives. |
| `MAPLEPC` | `maple_jvs.cpp` `MIEImpl::handle_86_subcommand()` | `MAPLEPC cmd=86 sub=%02x pc=%08x` | Guest PC for each MIE (Maple JVS bridge) subcommand: `sub=15`/`33` = input polls, `01`/`03`/`0b` = EEPROM read/write → locates the **input and EEPROM functions** the port shims to Dreamcast equivalents. |
| `BIOSEXEC` | `core/hw/sh4/interpr/sh4_interpreter.cpp` fetch path | `BIOSEXEC pc=%08x` | Any instruction executed inside Naomi BIOS ROM (phys `< 0x200000`) *after* the game's entry point (`0x8c04ae2c`, game-specific) has been reached — i.e. does the game **call back into the BIOS** after handoff? (It does: an EEPROM library at BIOS `0x60000` — a major porting obstacle.) Deduplicated per PC. |

### Phase 4 — debugging the converted Dreamcast build

The DC build replaces cart DMA with a **shim**: a small freestanding runtime
placed at phys `0x0cfc0000–0x0cffffff`, which services a fake "G1 register
mirror" block at phys `0x0cfc8800–0x0cfc9000` (the patched game writes its DMA
programming there instead of to real G1 registers; `mirror+0x418` mirrors the
`SB_GDST` "go" trigger). Several probes exist purely to watch that mechanism.

| Tag | Where | Line format | Question it answers |
|---|---|---|---|
| `SHIMWATCH` | `naomi.cpp`, every 64th DMA | `SHIMWATCH addr=%08x` | Fired (once) if *anything* writes a non-zero byte into the planned shim home `0x0cfc0000–0x0cffffff` during a Naomi-mode run — proving the game never touches the region before the port claims it. Deliberately a **RAM content scan, not a write hook**: the ARM64 dynarec's fast memory path stores straight into host RAM, bypassing every C-level write function, so a hook would silently miss most writes. |
| `MIRRORWR` | `core/hw/mem/addrspace.cpp` `writet()` | `MIRRORWR pc=%08x off=%03x val=%08x` | Every guest store into the G1 mirror block, with the storing PC — captures the patched game's DMA programming sequence (cart offset, destination, length, trigger). |
| `MIERESP` | `core/hw/maple/maple_if.cpp` `maple_DoDma()` | `MIERESP sub=%02x addr=%08x data=<128 hex chars>` | The MIE's **actual reply bytes** (fixed 0x40-byte dump) plus the guest receive address, for JVS `0x86` transactions — captured *after* `RawDma` and the byte-order fixup, i.e. byte-identical to what lands in guest RAM. The DC shim synthesizes these replies; this is its reference corpus. (The reply buffer is zeroed first so the dump never leaks uninitialized host stack bytes.) |
| `CLEO-MIE` | `maple_jvs.cpp` `BaseMIE::RawDma()` (both reply paths) | `CLEO-MIE cmd=%02x hdr_in=%08x replylen=%d reply=<hex>` | Same idea for **non-0x86** MIE commands (reset, GetID, Z80 firmware upload…): the full init ladder the game runs on real hardware, byte-exact, so the shim can service it. |

#### Boot-hang dissection kit (Task 13)

The DC build initially froze at boot with a black screen. These probes
identify *where* a guest is stuck, whichever way it is stuck:

| Tag | Where | Line format | Catches |
|---|---|---|---|
| `PCSAMPLE` | `sh4_interpreter.cpp` fetch path | `PCSAMPLE pc=%08x gdst_mirror=%08x` | Wall-clock 1 Hz sample of the fetched PC (plus the shim's mirrored `SB_GDST` word). Placed in the *fetch* path, not the outer timeslice loop, because a real-time-paced inner loop rarely reaches the outer loop — an outer-loop sampler starves. |
| `HANG` | same | `HANG pc=%08x gdst_mirror=%08x` | **Busy spin**: >50 M consecutive fetches inside one <32-byte PC window (e.g. a `bf`-to-self DMA-completion poll). Threshold far above any bounded init loop, crossed near-instantly by a real spin. One-shot. |
| `SLEEPWAIT` | `core/hw/sh4/interpr/sh4_opcodes.cpp` `SLEEP` opcode | `SLEEPWAIT pc=%08x pr=%08x gdst_mirror=%08x` | **Paced idle**: a guest parked on `SLEEP` barely advances the fetch count, starving both probes above; the emulator spends real seconds inside each `SLEEP`. Logged at the opcode itself, ~1/sec, with `pr` = who called the wait routine. |
| `EXC` | `sh4_interpreter.cpp` `Run()` exception path | `EXC epc=%08x evn=%03x newpc=%08x vbr=%08x` | **Exception storm**: a fault taken every instruction (e.g. before the guest installs VBR handlers) short-circuits the inner loop and bypasses `PCSAMPLE`. Logs faulting PC, event code, vector, ~1/sec. |
| `HWR` / `HWW` | `addrspace.cpp` `readt()`/`writet()` | `HWR pc=%08x addr=%08x val=%08x` | **Stuck inside one MMIO access**: guest MMIO reads/writes from the game's own code region (phys `0x0c020000–0x0c200000`, filtering out loader/BIOS noise), RAM accesses skipped, consecutive identical accesses collapsed. If the PC sampler freezes while the CPU burns, the *last* `HWR`/`HWW` line names the register whose emulator-side handler is looping. |
| `MDODMA*` | `maple_if.cpp` `maple_DoDma()` | `MDODMA enter/rawdma_call/rawdma_ret/frame_done ...`, `MDODMA_RUNAWAY iters=%u addr=%08x hdr1=%08x` | The actual culprit class found: `maple_DoDma` runs **synchronously** from the guest's `SB_MDST=1` store and walks the descriptor list in an unbounded `while(!last)` loop — a malformed list (no end-bit) freezes the guest inside a single store opcode. Entry/exit bracketing pins a hang to one Maple frame; a **100 000-iteration guard** aborts the walk, clears `SB_MDST`, and logs, so the run survives to show what happens next. *(This guard is a real behavior change — see below.)* |

#### "I/O BD IS NOT CONNECTED" hunt (Task 14c)

The converted game booted but sat on an I/O-board error screen. Two probes,
both heavily game-specific (addresses from Ghidra analysis of this game):

| Tag | Where | Line format | Purpose |
|---|---|---|---|
| `STRWATCH` | `addrspace.cpp` `readt()` | `STRWATCH pa=%08x pc=%08x pr=%08x` | The error strings live at phys `0x0c0ca6dc–0x0c0ca740` with **no static xref** (computed resource offset). Trap any read of those bytes and log reader PC + PR → finds the text-draw call chain, working back to the decision. Deduped per address. |
| `IOCHK` | `sh4_interpreter.cpp` fetch path | `IOCHK pc=%08x r0=%08x obj=%08x m10=%08x m7c=%08x conn=%08x specs=%08x mir=%08x` | Breakpoint-style snapshot at four PCs inside the game's scene loop (`0x0c04b08a/90/1fa/200`): the scene object (`*0x8c0c4510`), two of its vtable-method results, the I/O-enumeration flags, and `r0` (the just-returned gate value) → identifies which method gates the error scene. |
| `CLEO-WATCH` | `addrspace.cpp` `writet()` | `CLEO-WATCH [%08x] = %08x pc=%08x pr=%08x` | Data watchpoint on two game variables (`0x0c0e842c`, `0x0c0e6298`): who writes them, from where. |

### Hardware-behavior probes (`CLEO-*` in the standard log)

Low-volume diffs of "what did the game program vs. what does a Dreamcast
expect", used to reconcile Naomi 31 kHz arcade video / JVS I/O / cabinet GPIO
with their DC equivalents:

| Tag | Where | Logs |
|---|---|---|
| `CLEO-SPG` | `core/hw/pvr/pvr_regs.cpp` `pvr_WriteReg()` | Every **change** to the video-timing registers (`SPG_LOAD`, `FB_R_CTRL` incl. the vclk divider bit, `VO_CONTROL`, `SPG_HBLANK/VBLANK/WIDTH`, `VO_STARTX/Y`) with the writing PC/PR — who sets up display timing, and to what. |
| `CLEO-GPIO` | `core/hw/sh4/modules/bsc.cpp` | Changes to the SH4 `PDTRA`/`PCTRA` GPIO ports with PC. On a Dreamcast these implement **video-cable detection**; on Naomi they mean something else entirely — a classic conversion trap. (`PCTRA` gets explicit read/write handlers, functionally identical to the plain register it replaces, plus logging.) |
| `CLEO-CCR` | `core/hw/sh4/modules/ccn.cpp` | Cache-control register writes (masking out the transient invalidate bits) — spots cache/OC-RAM reconfiguration by the game. |
| `CLEO-ARMRST` | `core/hw/aica/aica_if.cpp` | AICA ARM reset writes plus the first two words of audio RAM — was a sound driver actually uploaded before the ARM was released from reset? (Also raises the existing message from INFO to NOTICE.) |

### Headless framebuffer → PNG (Task 18)

`core/ui/gui.cpp` `gui_dumpFramebuffer()`, called from
`core/ui/mainui.cpp` after each rendered frame. Lets an unattended run (or an
AI agent driving the emulator) verify what is on screen **without any OS
screen-capture API/permission** — it reuses Flycast's own screenshot readback
(`getScreenshot()` → renderer `GetLastFrame()`), so it reads the GL
framebuffer directly.

- Enable: set `FLYCAST_SHOT=/abs/path/shot.png` before launch. Disabled (and
  zero-cost) otherwise.
- Triggers: every `FLYCAST_SHOT_EVERY` frames (default 60, overwriting the
  same file), **and** on `SIGUSR1` (`kill -USR1 <pid>`) for an on-demand grab
  — the reliable way to capture a *specific* screen; the periodic dump often
  samples a transition/black frame.
- Output: 640×480 8-bit RGB PNG.

## Behavior changes (everything else is pure logging)

1. **`maple_DoDma` runaway guard** (`maple_if.cpp`): after 100 000 descriptor
   entries the walk is aborted, `SB_MDST` is cleared and `MDODMA_RUNAWAY`
   logged. Upstream loops forever on such a list; this build escapes so the
   log can show where the guest goes next. Diagnostic escape hatch, not a fix.
2. **Reply-buffer zeroing** in `maple_DoDma`: guest-visible bytes are
   unchanged (only `outlen` bytes were ever copied out); it only keeps the
   `MIERESP` fixed-size dump from reading uninitialized host stack.
3. **`BSC_PCTRA` handler swap** (`bsc.cpp`): plain auto-RW register → explicit
   handlers with identical semantics, plus logging.
4. **macOS build fixes** (not instrumentation): top-level `CMakeLists.txt`
   gains `enable_language(OBJC)` (SDL2 subproject compiles `.m` files but
   never enables OBJC — breaks under command-line tools + CMake 3.31) and the
   MoltenVK bundling step is skipped unless Vulkan is enabled and an SDK is
   present (this instrumentation build uses OpenGL).
   `patches/flycast-syphon-build-fix.diff` must additionally be applied
   *inside* the `core/deps/Syphon` submodule (submodules can't carry fork
   commits): it makes Syphon's ObjC prefix-header PCH `PRIVATE` so it doesn't
   poison Flycast's OBJC++ sources.

## Game-specific constants

The *techniques* are generic; these hardcoded values are specific to the
Cleopatra Fortune Plus binary or to this port's layout. To reuse a probe on
another game, re-derive them:

| Constant | Used by | Meaning |
|---|---|---|
| `0x8c04ae2c` | `BIOSEXEC` arming | Game entry point (BIOS→game handoff). |
| phys `0x0c020000–0x0c200000` | `HWR`/`HWW` filter | The game's own code region (loads at `0x8c020000`); filters loader/BIOS noise. |
| phys `0x0cfc0000–0x0cffffff` | `SHIMWATCH` | Port-defined shim home. |
| phys `0x0cfc8800–0x0cfc9000`, trigger `+0x418` | `MIRRORWR`, `gdst_mirror=` fields | Port-defined G1 register-mirror block serviced by the shim. |
| phys `0x0c0ca6dc–0x0c0ca740` | `STRWATCH` | "I/O BD IS NOT CONNECTED" string block. |
| `0x0c04b08a/90/1fa/200`, `0x8c0c4510`, `0x8c1c9774`, `0x8c0d541c`, `0x8c127b0c` | `IOCHK` | Scene-loop test PCs, scene-object pointer, I/O-enumeration flags. |
| phys `0x0c0e842c`, `0x0c0e6298` | `CLEO-WATCH` | Two watched game variables. |

## Interpreter vs dynarec

Probes that read the guest PC at fetch/memory-access time only see exact,
per-instruction state on the **interpreter**; the ARM64 dynarec's fast memory
path also bypasses `readt`/`writet` entirely for RAM and can skip the hooked
paths. For any capture involving `CARTDMAPC`, `MAPLEPC`, `BIOSEXEC`, `HANG`,
`PCSAMPLE`, `SLEEPWAIT`, `EXC`, `HWR`/`HWW`, `MIRRORWR`, `STRWATCH`, `IOCHK`
(and for trustworthy `pc=` fields anywhere), disable the dynarec:
`Dynarec.Enabled=no` under `[config]` in `emu.cfg`, or select the Interpreter
in the GUI's CPU settings. Expect ~10× slowdown.

Device-level probes (`CARTDMA`, `CARTPIO`, `WATERMARK`, `SHIMWATCH`,
`SERIALPOKE`, `JVSREPORT`, `MIERESP`, `MDODMA*`, `CLEO-MIE`, `CLEO-SPG`,
`CLEO-GPIO`, `CLEO-CCR`, `CLEO-ARMRST`) fire under the dynarec too — that is
exactly why `SHIMWATCH` is a content scan rather than a write hook.

## Building (macOS notes)

General build instructions are upstream's. This fork was built on macOS/arm64,
which needed, beyond the in-tree CMake fixes:

- **CMake 3.31.x** (Kitware binary) — Homebrew CMake 4.x fails at generate on
  this Flycast base (cmrc/OBJC).
- Full Xcode via `export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer`
  (OBJC ABI detection needs `xcodebuild`; CommandLineTools alone fails).
- Submodules first, then the Syphon patch **inside the submodule**:

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

Unattended-run gotchas found the hard way (macOS): launch with
`-config config:rend.vsync=no` or the emu thread can deadlock once the window
loses focus, and
`defaults write com.flyinghead.Flycast ApplePersistenceIgnoreState -bool YES`
or a previously killed instance makes the *next* launch block on an invisible
"reopen windows?" state (process alive at ~0 % CPU, guest never boots).

## Typical capture session

```sh
FLYCAST_CARTLOG=/tmp/capture.log \
FLYCAST_SHOT=/tmp/shot.png FLYCAST_SHOT_EVERY=60 \
  ./build/flycast -config config:rend.vsync=no /path/to/game &
# ... let it boot/attract/play ...
kill -USR1 %1        # on-demand screenshot
grep '^CARTDMA '  /tmp/capture.log | sort -u   # cart-streaming map
grep '^WATERMARK' /tmp/capture.log | tail -3   # RAM high-water marks
grep '^JVSREPORT' /tmp/capture.log | uniq -c   # input words seen
```
