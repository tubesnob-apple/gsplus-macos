# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

GSplus is a cross-platform Apple IIgs emulator based on KEGS (Kent's Emulated GS) by Kent Dickey. It emulates the 65816 CPU, all Apple IIgs graphics/sound modes, disk controllers, serial ports, and more. Licensed under GPLv3.

## Repository Structure

- `gsplus/src/` — Active source code and build files (this is where you build)
- `gsplus/lib/` — Icons, NIB files, and asset resources
- `upstream/kegs/` — Upstream KEGS tracked separately, merged to main when updated
- `upstream/kegs/doc/` — Comprehensive documentation (architecture internals, platform setup, compatibility)

The `upstream` branch tracks KEGS releases; `main` is the primary development branch.

## Build Commands

### macOS (default target)
```bash
cd gsplus/src
make -j 20
```
Produces `gsplus/GSplus.app`. Requires Xcode with command-line tools installed.

### Linux (X11)
```bash
cd gsplus/src
rm vars; ln -s vars_x86linux vars
make -j 20
```
Produces `xkegs`. Requires `libX11-devel`, `libXext-devel`, `pulseaudio-libs-devel`.

### Windows
Open `gsplus/src/kegswin.vcxproj` in Visual Studio Community Edition and press F7.

### Clean
```bash
cd gsplus/src
make clean
```

## Architecture

### CPU & Core Loop
- `sim65816.c` — Main simulation loop, event scheduling, interrupt handling
- `engine_c.c` + `engine.h` — 65816 CPU instruction emulation (macro-heavy for performance)
- `defs_instr.h`, `instable.h`, `op_routs.h` — Instruction definitions, opcode table, operation macros

### Memory
- `moremem.c` — Memory management, page table fixup, I/O register reads/writes ($C000-$C0FF area)
- Page-table-based MMU for dynamic address mapping

### Video
- `video.c` — All Apple IIgs/II graphics mode rendering (text, lores, hires, super hires)

### Audio
- `sound.c` — Sound generation (mixing, output buffering)
- `doc.c` — Ensoniq DOC 32-voice synthesizer emulation
- `mockingboard.c` — Mockingboard A card (6522 VIA + AY-8913)

### Disk I/O
- `iwm.c` — IWM disk controller (5.25" and 3.5" drives, nibble-level accuracy)
- `smartport.c` — SmartPort controller for hard drive images
- `dynapro.c` — Host directory mounting as virtual ProDOS volumes
- `woz.c` — WOZ disk image format support
- `unshk.c`, `undeflate.c`, `applesingle.c` — Archive/compression format support

### Input
- `adb.c` — Apple Desktop Bus (keyboard, mouse)
- `scc.c` + `scc_socket_driver.c` — Serial Communications Controller with TCP/IP modem emulation
- `paddles.c`, `joystick_driver.c` — Game input

### Configuration & Debug
- `config.c` — Runtime configuration UI, disk mounting, settings persistence (`config.kegs`)
- `debugger.c` — Built-in 65816 debugger/monitor
- `DebugConsoleWindow.swift` — New debug console UI (pure Swift class, auto-opens on halt)

### Platform Drivers
The emulator core is platform-independent. Platform-specific code is isolated in driver files:

| Component | macOS | Linux | Windows |
|-----------|-------|-------|---------|
| Display | `AppDelegate.swift` + `MainView.swift` | `xdriver.c` | `windriver.c` |
| Audio | `macsnd_driver.c` (CoreAudio) | `pulseaudio_driver.c` | `win32snd_driver.c` |
| Serial | `scc_unixdriver.c` | `scc_unixdriver.c` | `scc_windriver.c` |

### Key Headers
- `defc.h` — Global defines, structs, macros (included by nearly every .c file)
- `defcomm.h` — Shared defines for C and assembly
- `protos_base.h` — Function prototypes for all modules

## Build System Details

The Makefile includes `vars` (platform config) and `ldvars` (object file list). To change platforms, symlink the appropriate vars file to `vars`:

| File | Platform |
|------|----------|
| `vars_mac` | macOS native (Swift UI + CoreAudio) |
| `vars_mac_x` | macOS with X11 display (CoreAudio, no Swift UI) |
| `vars_x86linux` | Linux X11 |

Swift files are compiled via the `comp_swift` wrapper script using Swift 4 with `-Onone`. The `dependency` file contains header dependency rules.

Compiler flags are set in `vars`: `-Wall -O2 -DMAC` for macOS. The `-DMAC` define selects macOS-specific code paths throughout the codebase.

**Swift-C bridge**: `Kegs-Bridging-Header.h` imports `defc.h`, exposing C structs/globals to Swift under the module name `Kegs`. This is how `AppDelegate.swift` and `MainView.swift` access emulator state directly.

**App signing**: The Makefile automatically ad-hoc signs `GSplus.app` via `codesign --force --deep --sign -` after linking. Swift runtime frameworks are copied into `Contents/Frameworks/` by the `cp_gsplus_libs` Perl script.

**compile_time.c**: Regenerated on every build using `__DATE__`/`__TIME__`, so it always recompiles — this is expected.

**Windows Mingw32** (`gspluswin.exe`): Currently broken and non-functional. Use the Visual Studio project instead.

## Runtime Requirements

The emulator needs an Apple IIgs ROM file to run. Demo disk images (`NUCLEUS03.gz`, `XMAS_DEMO.gz`) are included in `upstream/kegs/`. Configuration is stored in `config.kegs`.

## No Test Suite

There is no automated test infrastructure. Testing is done manually by running Apple IIgs software and ROM self-tests.

## Known Issues

`gsplus/src/TODO.md` tracks known issues and planned work. Check it before starting work on a new feature or bug fix.

## WDM Debug Trap System

GSplus repurposes the 65816 WDM opcode ($42) as a debug trap mechanism for GS/OS software under development.

### Operand split
- **$00–$7F** → DBG trap path (`do_dbg()` via `RET_DBG`)
- **$80–$FF** → legacy WDM path (`do_wdm()` via `RET_WDM`)

### DBG trap behaviour
- **WDM $00**: emits only the debug string + newline (no adornment, no registers); never halts. Use as a lightweight `printf` from IIgs code.
- **WDM $01–$0F**: prints `WDM #$xx at BB/OOOO : {string}` + register dump; never halts. Use for informational breakpoints that should not stop execution.
- **WDM $10–$7F**: same output as $01–$0F; halts the emulator (calls `set_halt_act(2)`) unless disabled via `g_wdm_trap_enabled`. Use for breakpoints that should stop the CPU.

### Per-slot debug string buffer (bank $D0)
`g_debug_buf[0x10000]` is mapped read/write to the entire $D0 bank via the page table (set up in `moremem.c:setup_pageinfo()`). It is invisible to GS/OS.

Each WDM operand $00–$7F has its own 256-byte slot:
- **Slot N** = `g_debug_buf[N * 0x100]`, i.e. IIgs address `$D0/N*100`
- Strings are capped at 255 chars (byte 255 treated as null terminator)
- After printing, the **full 256 bytes** of the slot are zeroed

GS/OS code writes a null-terminated string to `$D0/xx00` before executing `WDM $xx`. The emulator prints it alongside the register dump.

### Trap enable/disable
`g_wdm_trap_enabled[128]` (in `sim65816.c`): only consulted for WDM $10–$7F (the halting range). Indices $10–$7F default to 1 (enabled). WDM $00–$0F never halt regardless of this array. Controllable via `set_wdm_trap` MCP tool.

### Debug console
`DebugConsoleWindow.swift` (`DebugConsoleWindowController`) auto-opens whenever `g_halt_sim > 0`. The old built-in debugger window is suppressed on halt (commented out in `engine_c.c:set_halt_act()`).

## Live Emulator Debugging (MCP)

When the user mentions any of the following — **emulator**, **gsplus**, **kegs**, **IIgs**, **running**, **crash**, **breakpoint**, **memory**, **registers**, **disk image**, **.2mg**, **ProDOS**, **volume**, **WDM**, **trap** — in a debugging or investigative context, assume they want to interact with the live running emulator via the `gsplus` MCP server.

**Automatically use the MCP tools** without waiting to be asked. Do not describe what you *could* do — just do it.

Typical trigger phrases and the right first tool call:
- "what's the emulator doing" / "where is it" → `halt_emulator` then `get_registers`
- "it crashed" / "it halted" / "something went wrong" → `get_break_info`
- "what's in memory at X" → `read_memory`
- "what's on the disk" / "what files" / "check the volume" → `list_volumes` then `read_volume`
- "disassemble" / "show the code" → `debugger_command` with `l BB/OOOO`
- "set a breakpoint" → `debugger_command` with `BB/OOOOB`
- "what's on screen" / "what did GNO print" → `get_screen_text`
- "run until" / "wait for address" / "boot to X" → `run_until`
- "write to memory" / "patch" → `write_memory`
- "restart" / "warm reset" → `restart_emulator`
- "cold reset" / "new kernel build" / "power cycle" → `cold_reset`
- "show the log" / "what was it doing before the crash" → `get_log`
- "enable/disable WDM trap" / "which traps are active" → `set_wdm_trap` / `get_wdm_traps`
- "type this" / "send keys" / "enter the command" → `send_keys`

Full tool catalog (21 tools):

| Tool | Purpose |
|------|---------|
| `get_registers` | Current 65816 CPU state |
| `read_memory` | Read any 24-bit address range (hex dump included) |
| `write_memory` | Write bytes to any 24-bit address |
| `search_memory` | Scan a range for a byte pattern |
| `halt_emulator` | Pause CPU |
| `continue_emulator` | Resume after halt/breakpoint |
| `step_emulator` | Single-step one instruction |
| `get_break_info` | CPU state at most recent crash/halt |
| `debugger_command` | Raw debugger text command (disassemble, memory, breakpoints) |
| `run_until` | Set temp BP, resume, block until hit or timeout (use poll_interval_s ≥3.0) |
| `get_screen_text` | Read 40/80-col text screen from shadow RAM ($E0/$E1 banks) |
| `list_volumes` | All mounted disk images and slot labels |
| `list_files` | Browse a ProDOS directory (recursive option available) |
| `read_file` | Read one file from a mounted volume |
| `read_volume` | Extract full ProDOS filesystem to host temp dir |
| `read_volume_raw` | Save raw disk image binary to host file |
| `restart_emulator` | Warm reset (ROM reset vector, ~16ms) |
| `cold_reset` | Full cold reset — clears RAM then warm-resets. Required when loading a new GNO kernel build (old kernel stays resident through warm resets via SetTSPtr) |
| `relaunch_app` | Kill and relaunch the entire GSplus host process for a true fresh start. Use this to reproduce startup hangs that only appear on fresh app launch (not on in-process cold_reset). Blocks until socket is ready (~5–10s). |
| `get_log` | Last N lines from 128KB dbg_printf ring buffer |
| `set_wdm_trap` | Enable/disable halt for WDM operands $01–$7F. `trap` = int, comma-separated string, or `"all"`. `enabled` = bool (default true). WDM $00 always ignored. |
| `get_wdm_traps` | Return enabled/disabled state of all 128 WDM trap slots as a 0/1 array |
| `send_keys` | Inject keystrokes into the keyboard buffer via the paste mechanism. Supports C-style escapes: `\n`, `\r` → Return, `\xHH` → hex byte, `\t` → tab. Emulator must be running. |

The MCP server connects to a socket at `/tmp/gsplus_debug.sock`. If GSplus is not running the tools will return an error saying so — report that to the user and stop.

## Swift 4 NSObject Subclass Ivar Layout (IMPORTANT)

**Problem:** In Swift 4 compatibility mode (`-swift-version 4 -Onone`, per-file compilation via `comp_swift`), NSObject subclasses with many Swift-typed stored properties (String, Array, Int, Bool, Optional) can produce corrupted ivar layouts at runtime. This causes EXC_BAD_ACCESS crashes in Swift ARC operations (`objc_release`, `doDecrementSlow`) on startup.

**Affected file:** `gsplus/src/DebugConsoleWindow.swift` — `DebugConsoleWindowController` (NSObject subclass with ~13 Swift stored properties) crashes on every launch.

**Rule:** Do NOT add NSObject subclasses with many Swift stored properties to the Swift code in this project. If a class needs to be a controller (not a view/NSObject), make it a **pure Swift class** (no NSObject inheritance) and use block-based `NotificationCenter` observers instead of `NSWindowDelegate` / `@objc` callbacks. Always add an explicit `init()` that initializes every stored property — do not rely on the synthesized init in Swift 4 per-file compilation mode.
