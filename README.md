# GSPlus for MacOS

A native macOS Apple IIgs emulator with integrated MCP-based debugging tools, built for developers working on GS/OS and GNO software.

## Overview

GSPlus for MacOS is an Apple IIgs emulator that combines cycle-accurate hardware emulation with modern AI-assisted debugging. It emulates the 65816 CPU, all Apple IIgs graphics and sound modes, disk controllers, serial ports, and more — running natively on macOS with a Swift-based UI and CoreAudio sound.

The standout feature of this version is a built-in **MCP (Model Context Protocol) debug server** that exposes the full emulator state to AI assistants like Claude. This allows conversational debugging of Apple IIgs software: inspecting registers, reading memory, setting breakpoints, browsing disk images, and injecting keyboard input — all through natural language.

## Building

Requires Xcode with command-line tools installed.

```bash
cd gsplus/src
make -j 20
```

Produces `gsplus/GSplus.app`.

## Running

The emulator requires an Apple IIgs ROM file. Configuration is stored in `config.kegs`. Launch `GSplus.app` and configure your ROM path and disk images through the built-in configuration UI (F4).

## What's New in This Version

### MCP Debug Server
A Unix socket-based debug server runs in a background thread, allowing external tools to inspect and control the emulator in real time. See [`debugmcp/README.md`](debugmcp/README.md) for full documentation.

### WDM Debug Trap System
The 65816 WDM opcode ($42) is repurposed as a debug trap mechanism with three tiers:

- **WDM $00** — Lightweight printf: emits the debug string with a PC address prefix, no register dump, never halts
- **WDM $01-$0F** — Informational breakpoints: full address/string output with register dump, never halts
- **WDM $10-$7F** — Halting breakpoints: full output with register dump, halts the emulator (can be individually enabled/disabled)

Each WDM operand has a 256-byte string buffer mapped to bank $D0. GS/OS code writes a null-terminated string to `$D0/xx00` before executing `WDM $xx`, and the emulator prints it alongside the trap output.

### Debug Console Window
A dedicated debug console window auto-opens whenever the emulator halts (breakpoint, WDM trap, or manual halt). Built as a pure Swift class to avoid Swift 4 runtime compatibility issues.

### Keyboard Injection
The `send_keys` MCP tool injects keystrokes into the emulator's keyboard buffer, enabling automated testing and scripted interaction with running Apple IIgs software.

## Project Structure

```
gsplus/src/          Source code and build files
gsplus/lib/          Icons, NIB files, and asset resources
debugmcp/            MCP debug server (Python + C backend)
upstream/kegs/       Upstream KEGS source, tracked separately
upstream/kegs/doc/   Comprehensive documentation
```

## License

This project is licensed under the GNU General Public License v3. See [LICENSE](LICENSE) for details.

## Acknowledgments

GSPlus for MacOS stands on the shoulders of decades of Apple II emulation work:

**KEGS** by Kent Dickey — The foundation of this emulator. Kent created KEGS (Kent's Emulated GS) and maintained it for over two decades, building a remarkably accurate Apple IIgs emulator that faithfully reproduces the 65816 CPU, Ensoniq DOC sound, IWM disk controller, and all Apple IIgs graphics modes. His careful attention to hardware accuracy made everything built on top of it possible. KEGS is available at [kegs.sourceforge.net](http://kegs.sourceforge.net/).

**GSplus / digarok** — This project is a fork of the [GSplus repository](https://github.com/digarok/gsplus) by Dagen Brock (digarok), who established the cross-platform GSplus project structure, build system, and project rigging that made it straightforward to extend the emulator with new capabilities.

Thank you to the broader Apple II community for keeping these machines alive and the software ecosystem thriving.
