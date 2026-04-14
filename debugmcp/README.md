# GSPlus MCP Debug Server

The MCP (Model Context Protocol) debug server exposes the GSPlus emulator's internals to AI assistants and external tools over a Unix domain socket. It enables live inspection and control of a running Apple IIgs emulation session.

## Architecture

The system has two components:

- **C backend** (`debug_server.c` / `debug_server.h`) — A pthread-based Unix socket server compiled into GSplus.app. It accepts JSON commands on `/tmp/gsplus_debug.sock` and interacts directly with the emulator's CPU state, memory, and disk subsystems.

- **Python MCP bridge** (`gsplus_mcp.py`) — An MCP server that translates MCP tool calls into JSON commands for the C backend. This is what Claude Code (or any MCP client) connects to.

```
Claude Code  <-->  gsplus_mcp.py (MCP/stdio)  <-->  /tmp/gsplus_debug.sock  <-->  GSplus.app
```

## Requirements

- Python 3.10+
- `mcp` package: `pip install mcp`
- GSPlus.app running (the socket appears automatically on launch)

## Setup

### Claude Code

Add to your project's `.mcp.json`:

```json
{
  "mcpServers": {
    "gsplus": {
      "command": "python3",
      "args": ["/path/to/gsplus/debugmcp/gsplus_mcp.py"]
    }
  }
}
```

Or configure in Claude Code settings (`~/.claude/settings.json`) for global availability.

### Other MCP Clients

Any MCP-compatible client can use `gsplus_mcp.py` as a stdio-based MCP server. Run it directly:

```bash
python3 debugmcp/gsplus_mcp.py
```

The server communicates over stdin/stdout using the MCP protocol.

## Tools

### CPU State & Execution Control

| Tool | Description |
|------|-------------|
| `get_registers` | Read current 65816 CPU state: PC, A, X, Y, SP, DP, DB, PSR flags, halt status, and cycle count. |
| `halt_emulator` | Pause CPU execution immediately. |
| `continue_emulator` | Resume execution after a halt or breakpoint. |
| `step_emulator` | Single-step one 65816 instruction. |
| `get_break_info` | Capture the CPU state at the most recent crash or halt, including the halt reason and the address where execution stopped. |
| `run_until` | Set a temporary breakpoint at a 24-bit address, resume execution, and block until the CPU hits it or a timeout expires. Returns full register state plus hit/timed_out flags. Use `poll_interval_s >= 3.0` to avoid stalling disk I/O. |

### Memory

| Tool | Description |
|------|-------------|
| `read_memory` | Read a range of bytes from any 24-bit address. Returns base64-encoded data and a formatted hex dump. |
| `write_memory` | Write bytes to any 24-bit address. Accepts space-separated hex bytes (e.g. `"EA EA 00"`). |
| `search_memory` | Scan an address range for a byte pattern. Returns all matching addresses. |

### Debugger

| Tool | Description |
|------|-------------|
| `debugger_command` | Send a raw text command to the built-in 65816 debugger. Supports disassembly (`l BB/OOOO`), breakpoints (`BB/OOOOB`), memory display, and all other debugger commands. |

### Disk & Filesystem

| Tool | Description |
|------|-------------|
| `list_volumes` | List all mounted disk images with their slot assignments. |
| `list_files` | Browse a ProDOS directory on a mounted volume. Supports recursive listing. |
| `read_file` | Read a single file from a mounted ProDOS volume. Returns base64-encoded data with automatic text preview for TXT and SRC file types. |
| `read_volume` | Extract the full ProDOS filesystem of a mounted volume to a temporary host directory. |
| `read_volume_raw` | Save a raw disk image binary to a host file. |

### Screen & Input

| Tool | Description |
|------|-------------|
| `get_screen_text` | Read the 40-column or 80-column text screen from shadow RAM. Auto-detects column mode and active page from soft switches. Returns 24 fixed-width lines with all character positions preserved. See the 80-column quirk note below. |
| `send_keys` | Inject a string of characters into the keyboard buffer. Characters are fed one at a time as the emulated software reads the keyboard. Supports C-style escape sequences: `\n` and `\r` for Return, `\t` for Tab, `\xHH` for any hex byte (e.g. `\x03` for Ctrl-C, `\x1b` for Escape). |

### System Control

| Tool | Description |
|------|-------------|
| `restart_emulator` | Warm reset — jumps to the ROM reset vector. Equivalent to pressing Ctrl-Reset. |
| `cold_reset` | Full cold reset — clears all RAM then warm-resets. Use this when loading a new kernel build, since the old kernel stays resident through warm resets. |
| `relaunch_app` | Kill and relaunch the entire GSplus host process. Use this to reproduce issues that only appear on fresh app launch. Blocks until the debug socket is ready. |

### Logging & Diagnostics

| Tool | Description |
|------|-------------|
| `get_log` | Retrieve the last N lines from the 128KB diagnostic ring buffer. Captures all `dbg_printf()` output including WDM trap messages. |

### WDM Debug Traps

| Tool | Description |
|------|-------------|
| `set_wdm_trap` | Enable or disable the halt behavior for WDM operands $10-$7F. Accepts a single trap number, a comma-separated list, or `"all"`. |
| `get_wdm_traps` | Return the enabled/disabled state of all 128 WDM trap slots as an array. |

## Prompts

The MCP server includes three built-in prompt templates for common workflows:

| Prompt | Description |
|--------|-------------|
| `debug_crash` | Guides crash investigation: get break info, disassemble surrounding code, inspect the stack. |
| `inspect_volume` | Dump the full filesystem tree of a mounted ProDOS volume. |
| `watch_execution` | Halt the emulator, show registers, disassemble at PC, and offer step/continue options. |

## Address Formats

All tools that accept addresses support three formats:

- **Bank/offset string**: `"01/AF02"` or `"e1/0400"`
- **Hex integer**: `0x01AF02`
- **Decimal integer**: `110338`

## Socket Protocol

The C backend uses a simple protocol on `/tmp/gsplus_debug.sock`:

1. Client connects (Unix stream socket)
2. Client sends a JSON object followed by a newline
3. Server sends a JSON response followed by a newline
4. Connection closes (one command per connection)

Example:
```json
{"cmd": "get_registers"}
```

Response:
```json
{"ok": true, "pc": "01/AF02", "acc": "0000", "xreg": "0000", ...}
```

## Notes & Quirks

### 80-column text screen: main/aux variable mislabel in `get_screen_text`

`get_screen_text` reads the Apple IIgs text page from shadow RAM. The
standard IIgs shadowing convention is:

- Bank `$00` (main memory) shadows into bank `$E0`
- Bank `$01` (aux memory)  shadows into bank `$E1`

In 80-column mode, the hardware pairs aux + main bytes column-by-column:
aux supplies the even display columns (0, 2, 4…) and main supplies the
odd display columns (1, 3, 5…).

**The implementation in `gsplus_mcp.py` has two compensating errors that
cancel out:**

1. The local variables `main_base` and `aux_base` are swapped relative
   to the hardware convention — `main_base` points at `$E10400` (which
   is actually aux) and `aux_base` points at `$E00400` (which is
   actually main).
2. The per-row emit loop then writes `main_byte` before `aux_byte`,
   which — given the swapped variable names — produces the correct
   aux-then-main visual order on screen.

The code works. But because the two wrongs cancel, fixing only one of
them will scramble 80-column output pair-by-pair. If you ever clean
this up, fix both in the same change: rename the variables to match
the hardware convention and flip the emit order to match.

Also: MouseText glyphs and inverse text are not distinguished from
their underlying ASCII in the output — bit 7 is stripped and the
remaining 7 bits are decoded as ASCII. This is intentional (text
stays readable) but means a Finder-style row of MouseText icons will
look like random `@ABC…` characters.

## Troubleshooting

- **"Connection refused" / socket not found** — GSplus.app is not running. Launch it first; the socket appears automatically.
- **Tools return stale data** — Some tools (like `get_screen_text`) read shadow RAM which is only updated while the emulator is running. Make sure the emulator is not halted, or `continue_emulator` first.
- **`send_keys` characters not appearing** — The emulator must be running (not halted) for keys to be consumed from the paste buffer. Use `continue_emulator` before sending keys.
- **`run_until` times out unexpectedly** — If `poll_interval_s` is too small (< 3.0), the repeated halt/resume cycles can interfere with disk I/O and prevent the emulator from reaching the target address.
