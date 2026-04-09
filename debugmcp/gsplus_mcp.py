#!/usr/bin/env python3
"""
GSplus Apple IIgs emulator debug MCP server.

Exposes emulator internals as MCP tools so another Claude session can
interactively debug a running .2mg disk image.

Usage:
    python3 gsplus_mcp.py

The emulator must be running (socket appears at /tmp/gsplus_debug.sock).

Configure in Claude Code settings:
    {
      "mcpServers": {
        "gsplus": {
          "command": "python3",
          "args": ["/path/to/debugmcp/gsplus_mcp.py"]
        }
      }
    }
"""

import asyncio
import base64
import json
import os
import socket
import subprocess
import time
from typing import Any

import mcp.types as types
from mcp.server import Server
from mcp.server.stdio import stdio_server

# ── Server instructions (shown to Claude at connection time) ──────────────

SERVER_INSTRUCTIONS = """
You are connected to a live GSplus Apple IIgs emulator via its debug endpoint.
GSplus emulates the 65816 CPU, all IIgs graphics/sound modes, ProDOS disk I/O,
and serial ports. The emulator is running independently; you connect to it
on demand through a Unix domain socket at /tmp/gsplus_debug.sock.

## Address format
All 65816 addresses are 24-bit: bank byte + 16-bit offset.
Tools accept addresses as:
  - "BB/OOOO"  bank/offset hex string  e.g. "01/2000"
  - 0xBBOOOO   integer                 e.g. 0x012000
  - decimal integer                    e.g. 73728

## Available tools and when to use them

### Crash / break investigation
1. `get_break_info`      — First call after any crash or unexpected halt.
                           Returns PC, all registers, PSR flags, and reason.
2. `get_registers`       — Current CPU state (safe to call while running).
3. `read_memory`         — Read any 24-bit address range (hex dump included).
4. `search_memory`       — Scan a range for a byte pattern (e.g. a JSR target).
5. `debugger_command`    — Run the built-in 65816 disassembler/debugger.
                           Use 'l BB/OOOO' to disassemble, 'm BB/OOOO' for memory.

### Execution control
6. `halt_emulator`       — Pause execution; all state is stable for inspection.
7. `continue_emulator`   — Resume after a halt or breakpoint.
8. `step_emulator`       — Execute exactly one 65816 instruction.

### Breakpoints (via debugger_command)
  Set:    "BB/OOOOB"          e.g. "01/2000B"
  List:   "B"
  Delete: "BB/OOOOB" (same)   or "BB/ADDR0.ADDR1D" for a range
  Clear all: "bp clear all"

### Disk / filesystem
9.  `list_volumes`       — What disk images are mounted and their slot labels.
10. `list_files`         — Browse a ProDOS directory tree on a mounted volume.
11. `read_file`          — Read a single file's raw bytes (base64) + metadata.
12. `read_volume`        — Full recursive filesystem dump (all files + data).
                           Use this to inspect the contents of a .2mg image.

### Emulator control
13. `restart_emulator`   — Warm reset (ROM reset vector). Same as pressing 'r'
                           in the debug window. Takes effect within ~16ms.
13b.`cold_reset`         — Full cold reset: clears all RAM then warm-resets.
                           Required for new GNO kernel builds (old kernel stays
                           resident through warm resets via SetTSPtr). Uses 'C'.
13c.`relaunch_app`       — Kill and relaunch the entire GSplus process for a true
                           fresh start (empty event queue, no carry-over state).
                           Blocks until the debug socket is back up (~5–10s).
14. `get_log`            — Last N lines of emulator diagnostic output from the
                           128KB ring buffer. Use for pre-crash context.
15. `write_memory`       — Write bytes to any 24-bit address. Cleaner than
                           crafting BB/OOOO:HH debugger_command strings by hand.
16. `run_until`          — Set a temp breakpoint, resume, and block until the
                           CPU hits that address (or timeout). Use poll_interval_s
                           ≥3.0 to avoid stalling disk I/O during boot.
17. `get_screen_text`    — Read the 40- or 80-col text screen from shadow RAM
                           ($E0/$E1 banks). Auto-detects column mode and active
                           page from emulator soft-switch state. Returns 24
                           fixed-width lines plus metadata (mode, page, width).
                           Safe to call while running. Best first-look tool.

## Common ProDOS file types
  $04 TXT   Text file          $06 BIN   Binary / code
  $0F DIR   Directory          $B3 S16   GS/OS 16-bit executable
  $FF SYS   ProDOS system file $B0 SRC   Source code

## Typical crash-debugging workflow
1. `get_break_info`           — capture PC and register state at crash
2. `debugger_command` "l BB/OOOO"  — disassemble around crash PC
3. `read_memory` at stack pointer  — inspect the call stack
4. `search_memory` for known constants — locate data structures
5. `list_volumes` → `read_file` or `read_volume` — inspect the disk image

## Slot labels
Volumes are referenced by slot label returned from `list_volumes`:
  sp0–sp15   SmartPort hard drive images (your .2mg will usually be sp0)
  s5d1, s5d2 3.5" floppy drives (slot 5)
  s6d1, s6d2 5.25" floppy drives (slot 6)
""".strip()

# ── Prompt templates ──────────────────────────────────────────────────────

PROMPTS = [
    types.Prompt(
        name="debug_crash",
        description=(
            "Start a crash investigation: captures break state, disassembles "
            "around the crash PC, and inspects the stack."
        ),
        arguments=[
            types.PromptArgument(
                name="context",
                description="Optional: what you were doing when it crashed",
                required=False,
            )
        ],
    ),
    types.Prompt(
        name="inspect_volume",
        description=(
            "List all mounted volumes and dump the full filesystem tree of "
            "the first ProDOS SmartPort volume (sp0)."
        ),
        arguments=[
            types.PromptArgument(
                name="slot",
                description="Volume slot to inspect (default: sp0)",
                required=False,
            )
        ],
    ),
    types.Prompt(
        name="watch_execution",
        description=(
            "Halt the emulator, show current registers, disassemble 20 "
            "instructions at the current PC, then offer to step or continue."
        ),
        arguments=[],
    ),
]

SOCK_PATH = "/tmp/gsplus_debug.sock"
TIMEOUT_S = 30.0   # seconds; read_volume on large images can take a moment

# ── Socket client ─────────────────────────────────────────────────────────

def _send(obj: dict) -> dict:
    """Send one JSON command to GSplus and return the parsed response."""
    try:
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.settimeout(TIMEOUT_S)
        sock.connect(SOCK_PATH)
        sock.sendall((json.dumps(obj) + "\n").encode())

        buf = b""
        while b"\n" not in buf:
            chunk = sock.recv(131072)
            if not chunk:
                break
            buf += chunk
        sock.close()
        return json.loads(buf.split(b"\n")[0])
    except FileNotFoundError:
        return {"ok": False,
                "error": f"Emulator not running — no socket at {SOCK_PATH}"}
    except Exception as exc:
        return {"ok": False, "error": str(exc)}


def _fmt(result: dict) -> str:
    return json.dumps(result, indent=2)


def _b64decode(s: str) -> bytes:
    return base64.b64decode(s)


def _hex_dump(raw: bytes, base_addr: int) -> str:
    lines = []
    for i in range(0, len(raw), 16):
        chunk = raw[i : i + 16]
        hex_part = " ".join(f"{b:02X}" for b in chunk)
        asc_part = "".join(chr(b) if 32 <= b < 127 else "." for b in chunk)
        lines.append(f"  {(base_addr + i):06X}:  {hex_part:<48}  {asc_part}")
    return "\n".join(lines)


def _parse_addr(addr) -> int:
    """Accept int, decimal str, 0x hex str, or 'BB/OOOO' bank/offset."""
    if isinstance(addr, int):
        return addr
    addr = str(addr).strip().strip('"').strip("'")
    if "/" in addr:
        bank, off = addr.split("/", 1)
        return (int(bank, 16) << 16) | int(off, 16)
    return int(addr, 0)


def _tree_summary(files: list, prefix: str = "") -> list[str]:
    lines = []
    for f in files:
        name = f.get("name", "?")
        ftype = f.get("type", "?")
        if ftype == "DIR":
            lines.append(f"{prefix}/{name}/")
            lines.extend(_tree_summary(f.get("files", []), prefix + "/" + name))
        else:
            eof = f.get("eof", 0)
            lines.append(f"{prefix}/{name}  [{ftype}, {eof} bytes]")
    return lines


# ── MCP server ────────────────────────────────────────────────────────────

server = Server("gsplus-debugger")

TOOLS: list[types.Tool] = [
    types.Tool(
        name="get_registers",
        description=(
            "Get all Apple IIgs 65816 CPU register values: PC (bank/addr), "
            "accumulator A, X, Y, stack pointer SP, direct page DP, data bank DB, "
            "processor status PSR with individual flag bits (N V M X D I Z C). "
            "Also reports whether the emulator is currently halted."
        ),
        inputSchema={"type": "object", "properties": {}, "required": []},
    ),
    types.Tool(
        name="read_memory",
        description=(
            "Read bytes from the emulated Apple IIgs 24-bit address space. "
            "Returns both a base64 blob and a formatted hex dump. "
            "addr accepts: integer (decimal or 0x hex), or 'BB/OOOO' bank/offset."
        ),
        inputSchema={
            "type": "object",
            "properties": {
                "addr": {
                    "description": "Start address (e.g. 0xE10000, '01/2000', 8192)",
                    "oneOf": [{"type": "integer"}, {"type": "string"}],
                },
                "len": {
                    "type": "integer",
                    "description": "Bytes to read (1–65536, default 256)",
                    "default": 256,
                },
            },
            "required": ["addr"],
        },
    ),
    types.Tool(
        name="search_memory",
        description=(
            "Scan an address range of the emulated Apple IIgs for a byte pattern. "
            "pattern is a hex string: '4c 00 c0' or '4c00c0'. "
            "Returns a list of matching 24-bit addresses in 'BB/OOOO' format."
        ),
        inputSchema={
            "type": "object",
            "properties": {
                "start": {
                    "description": "First address to search (inclusive)",
                    "oneOf": [{"type": "integer"}, {"type": "string"}],
                },
                "end": {
                    "description": "Last address to search (inclusive)",
                    "oneOf": [{"type": "integer"}, {"type": "string"}],
                },
                "pattern": {
                    "type": "string",
                    "description": "Hex bytes, e.g. '4c 00 c0'",
                },
            },
            "required": ["start", "end", "pattern"],
        },
    ),
    types.Tool(
        name="halt_emulator",
        description="Pause the emulator. CPU stops; registers are stable for inspection.",
        inputSchema={"type": "object", "properties": {}, "required": []},
    ),
    types.Tool(
        name="continue_emulator",
        description="Resume the emulator after a halt or breakpoint.",
        inputSchema={"type": "object", "properties": {}, "required": []},
    ),
    types.Tool(
        name="step_emulator",
        description="Execute one 65816 instruction, then halt again.",
        inputSchema={"type": "object", "properties": {}, "required": []},
    ),
    types.Tool(
        name="get_break_info",
        description=(
            "Return the CPU state captured at the most recent break, crash, or halt. "
            "Includes PC, all registers, PSR flags, and the break reason "
            "(BRK instruction, breakpoint, halt_printf/fatal, or explicit). "
            "Call this immediately after a crash to diagnose the root cause."
        ),
        inputSchema={"type": "object", "properties": {}, "required": []},
    ),
    types.Tool(
        name="debugger_command",
        description=(
            "Run a command in the built-in GSplus 65816 debugger. "
            "The emulator is halted while the command runs; output is captured "
            "and returned as text. Examples:\n"
            "  'l 01/2000'     disassemble at bank 01, offset 2000\n"
            "  'm 00/0300.03ff' show memory range\n"
            "  '01/2000B'      set execute breakpoint at 01/2000\n"
            "  'B'             list all breakpoints\n"
            "  '01/2000D'      delete breakpoint at 01/2000\n"
            "  'g'             go (resume)\n"
            "  's'             single step\n"
            "  'logpc on'      enable PC logging"
        ),
        inputSchema={
            "type": "object",
            "properties": {
                "text": {"type": "string", "description": "Debugger command string"},
            },
            "required": ["text"],
        },
    ),
    types.Tool(
        name="list_volumes",
        description=(
            "List all disk images currently mounted in the emulator. "
            "Returns slot label (e.g. 'sp0', 's5d1', 's6d1'), host file path, "
            "ProDOS volume name, total blocks, image type, and write-protect status. "
            "Use the slot label with list_files, read_file, and read_volume."
        ),
        inputSchema={"type": "object", "properties": {}, "required": []},
    ),
    types.Tool(
        name="list_files",
        description=(
            "List the contents of a ProDOS directory on a mounted volume. "
            "Call list_volumes first to get the slot label. "
            "Each entry includes name, ProDOS file type, byte size, and aux type. "
            "By default only the immediate directory contents are returned; "
            "set recursive=true to expand subdirectories."
        ),
        inputSchema={
            "type": "object",
            "properties": {
                "slot": {
                    "type": "string",
                    "description": "Volume slot from list_volumes (e.g. 'sp0')",
                },
                "path": {
                    "type": "string",
                    "description": "ProDOS directory path (default '/')",
                    "default": "/",
                },
                "recursive": {
                    "type": "boolean",
                    "description": "If true, expand subdirectories recursively (default false)",
                    "default": False,
                },
            },
            "required": ["slot"],
        },
    ),
    types.Tool(
        name="read_file",
        description=(
            "Read the raw contents of a file from a mounted ProDOS volume. "
            "Returns base64-encoded data plus metadata (type, EOF, aux). "
            "Text files ($04) also include a decoded text preview. "
            "Files larger than 1 MB are noted but not returned."
        ),
        inputSchema={
            "type": "object",
            "properties": {
                "slot": {
                    "type": "string",
                    "description": "Volume slot (e.g. 'sp0')",
                },
                "path": {
                    "type": "string",
                    "description": "Full ProDOS path (e.g. '/MYAPP/STARTUP')",
                },
            },
            "required": ["slot", "path"],
        },
    ),
    types.Tool(
        name="read_volume_raw",
        description=(
            "Read the raw binary disk image data from a mounted volume and save it "
            "as a .img file on the host machine. Returns the output file path, "
            "image type, and byte size — no binary data is sent over this connection. "
            "The saved file is the raw ProDOS block data (i.e. a .2mg stripped of its "
            "header, or a .dsk/.po as-is). Use this when you need to work with the "
            "volume image directly as a binary file."
        ),
        inputSchema={
            "type": "object",
            "properties": {
                "slot": {
                    "type": "string",
                    "description": "Volume slot from list_volumes (e.g. 'sp0')",
                },
            },
            "required": ["slot"],
        },
    ),
    types.Tool(
        name="read_volume",
        description=(
            "Extract the complete ProDOS filesystem of a mounted volume to a "
            "temporary directory on the host machine. No binary data is returned "
            "in this response — instead you get the output directory path, a "
            "manifest file path, total file count and byte count, and a plain-text "
            "directory tree. Use the output_dir path with normal file-reading tools "
            "to inspect individual files. Files over 1MB are listed in the tree but "
            "not extracted."
        ),
        inputSchema={
            "type": "object",
            "properties": {
                "slot": {
                    "type": "string",
                    "description": "Volume slot from list_volumes (e.g. 'sp0')",
                },
            },
            "required": ["slot"],
        },
    ),
    types.Tool(
        name="restart_emulator",
        description=(
            "Perform a warm reset of the emulated Apple IIgs — equivalent to "
            "pressing 'r' in the debug window. The emulator restarts from the "
            "ROM reset vector. Useful for recovering from crashes or testing "
            "startup sequences. The reset runs on the emulator's main thread "
            "and takes effect within ~16ms."
        ),
        inputSchema={"type": "object", "properties": {}, "required": []},
    ),
    types.Tool(
        name="cold_reset",
        description=(
            "Perform a full cold reset of the emulated Apple IIgs — clears all "
            "RAM (equivalent to a power cycle), then restarts from the ROM reset "
            "vector. Required when testing a new GNO kernel build: the old kernel "
            "stays resident through warm resets and kernStatus() will report "
            "'already active', preventing the new kernel from loading. "
            "Uses debugger command 'C'."
        ),
        inputSchema={"type": "object", "properties": {}, "required": []},
    ),
    types.Tool(
        name="relaunch_app",
        description=(
            "Kill the running GSplus process and relaunch it as a completely fresh "
            "app instance — equivalent to quitting and reopening from Finder. "
            "This gives a true cold start with fully reset host-process state "
            "(empty event queue, zeroed RAM, no carry-over from previous runs). "
            "The debug socket will be offline for a few seconds while GSplus "
            "restarts; this tool blocks until the socket is available again "
            "(up to wait_s seconds). Use this instead of cold_reset when you need "
            "to reproduce startup hangs that only appear on fresh app launch."
        ),
        inputSchema={
            "type": "object",
            "properties": {
                "wait_s": {
                    "type": "number",
                    "description": "Seconds to wait for socket to reappear (default 20)",
                    "default": 20,
                },
            },
            "required": [],
        },
    ),
    types.Tool(
        name="get_log",
        description=(
            "Return the last N lines of emulator diagnostic output (dbg_printf "
            "output captured into a 128 KB ring buffer). Includes all messages "
            "from the debugger, halt_printf calls, IWM/SCC/DOC state dumps, "
            "and any output produced by debugger_command calls. Useful for "
            "understanding what the emulator was doing before a crash."
        ),
        inputSchema={
            "type": "object",
            "properties": {
                "lines": {
                    "type": "integer",
                    "description": "Number of lines to return (1–1000, default 50)",
                    "default": 50,
                },
            },
            "required": [],
        },
    ),
    types.Tool(
        name="write_memory",
        description=(
            "Write bytes to the emulated Apple IIgs 24-bit address space using "
            "the built-in debugger's BB/OOOO:HH command (one byte per command). "
            "Use this instead of manually crafting debugger_command strings. "
            "addr accepts: integer (decimal or 0x hex), or 'BB/OOOO' bank/offset."
        ),
        inputSchema={
            "type": "object",
            "properties": {
                "addr": {
                    "description": "Start address (e.g. 0x08EEE9, '00/0300', 8192)",
                    "oneOf": [{"type": "integer"}, {"type": "string"}],
                },
                "data": {
                    "type": "string",
                    "description": "Space-separated hex bytes to write, e.g. \"EA EA 00\"",
                },
            },
            "required": ["addr", "data"],
        },
    ),
    types.Tool(
        name="run_until",
        description=(
            "Set a temporary execute breakpoint, resume the emulator, and block "
            "until the CPU hits that address or the timeout expires. Returns full "
            "register state plus hit/timed_out flags. "
            "Use this instead of manually set-BP / continue / poll-registers loops. "
            "poll_interval_s should be ≥3.0 — polling too fast stalls disk I/O "
            "because it repeatedly halts and resumes the emulator."
        ),
        inputSchema={
            "type": "object",
            "properties": {
                "addr": {
                    "description": "24-bit address to break on (e.g. 0x00C600, '00/c600')",
                    "oneOf": [{"type": "integer"}, {"type": "string"}],
                },
                "timeout_s": {
                    "type": "number",
                    "description": "Seconds to wait before giving up (default 60)",
                    "default": 60,
                },
                "poll_interval_s": {
                    "type": "number",
                    "description": "Seconds between halt-checks (default 3.0, minimum 1.0)",
                    "default": 3.0,
                },
            },
            "required": ["addr"],
        },
    ),
    types.Tool(
        name="set_wdm_trap",
        description=(
            "Enable or disable the emulator halt triggered by WDM instructions "
            "with operands in the range $01–$7F. WDM $00 is always non-breaking "
            "and cannot be changed. "
            "'trap' accepts a single integer, a list of integers, or the string "
            "\"all\" to affect every trap $01–$7F at once. "
            "'enabled' defaults to true."
        ),
        inputSchema={
            "type": "object",
            "properties": {
                "trap": {
                    "description": (
                        "Which trap(s) to change. Accepts: a single integer (e.g. 1), "
                        "a comma-separated list of integers (e.g. \"1, 2, 5\"), "
                        "a bracketed list (e.g. \"[1, 2, 5]\"), "
                        "or the string \"all\"."
                    ),
                    "oneOf": [{"type": "integer"}, {"type": "string"}],
                },
                "enabled": {
                    "type": "boolean",
                    "description": "True to enable halting (default), false to disable.",
                    "default": True,
                },
            },
            "required": ["trap"],
        },
    ),
    types.Tool(
        name="get_wdm_traps",
        description=(
            "Return the enabled/disabled state of all 128 WDM trap slots ($00–$7F). "
            "Returns a 128-element array of 0/1 values indexed by operand. "
            "Index 0 is always 0 (WDM $00 never halts)."
        ),
        inputSchema={"type": "object", "properties": {}, "required": []},
    ),
    types.Tool(
        name="get_screen_text",
        description=(
            "Read the current Apple IIgs text screen and return it as readable text. "
            "Reads shadow RAM in bank $E1 (main) and $E0 (aux) — safe to call while "
            "the emulator is running. Auto-detects 40/80-column mode and active page "
            "from the emulator's soft-switch state ($C01C/$C01F); omit 'col80' and "
            "'page' to use auto-detection (recommended). Returns 24 fixed-width lines "
            "with ALL character positions preserved (including spaces), plus a 'meta' "
            "object describing the detected mode, width, page, and detection source. "
            "Apple II character codes 0x20-0x7E map to standard ASCII after stripping "
            "the display-attribute bit (bit 7 = normal, bit 6-7 = 0 inverse, "
            "bit 6 = 1 MouseText). Control codes (0x00-0x1F) render as spaces."
        ),
        inputSchema={
            "type": "object",
            "properties": {
                "page": {
                    "type": "integer",
                    "description": (
                        "Text page 1 or 2. Omit to auto-detect from the emulator's "
                        "PAGE2 soft switch ($C01C) — recommended."
                    ),
                    "enum": [1, 2],
                },
                "col80": {
                    "type": "boolean",
                    "description": (
                        "True for 80-column, False for 40-column. Omit to auto-detect "
                        "from the emulator's VID80 soft switch ($C01F) — recommended."
                    ),
                },
            },
            "required": [],
        },
    ),
    types.Tool(
        name="send_keys",
        description=(
            "Inject a string of characters into the emulated Apple IIgs keyboard "
            "buffer, as if the user typed them. Uses the paste buffer mechanism "
            "(adb_paste_add_buf) so characters are fed one at a time as the "
            "emulated software reads $C000/$C010. Unix newlines (\\n) are "
            "automatically converted to Apple II returns (\\r). The emulator "
            "must be running (not halted) for the keys to be consumed."
        ),
        inputSchema={
            "type": "object",
            "properties": {
                "keys": {
                    "type": "string",
                    "description": (
                        "The string to type. Standard ASCII characters (0x01-0x7F). "
                        "Use \\r or \\n for Return. Control characters work too: "
                        "e.g. \\x03 for Ctrl-C, \\x1b for Escape."
                    ),
                },
            },
            "required": ["keys"],
        },
    ),
]


@server.list_tools()
async def list_tools() -> list[types.Tool]:
    return TOOLS


@server.list_prompts()
async def list_prompts() -> list[types.Prompt]:
    return PROMPTS


@server.get_prompt()
async def get_prompt(name: str, arguments: dict[str, str] | None) -> types.GetPromptResult:
    args = arguments or {}

    if name == "debug_crash":
        context = args.get("context", "")
        context_line = f"\nContext: {context}" if context else ""
        return types.GetPromptResult(
            description="Crash investigation starting point",
            messages=[
                types.PromptMessage(
                    role="user",
                    content=types.TextContent(
                        type="text",
                        text=(
                            f"The emulator has crashed or halted.{context_line}\n\n"
                            "Please investigate by:\n"
                            "1. Calling `get_break_info` to capture the crash state\n"
                            "2. Calling `debugger_command` with 'l BB/OOOO' to disassemble "
                            "20+ instructions around the crash PC (substitute the actual bank/offset)\n"
                            "3. Calling `read_memory` at the stack pointer (SP from get_break_info) "
                            "to show the top 64 bytes of the stack\n"
                            "4. Summarising what you think went wrong based on the evidence"
                        ),
                    ),
                )
            ],
        )

    elif name == "inspect_volume":
        slot = args.get("slot", "sp0")
        return types.GetPromptResult(
            description="Volume filesystem inspection",
            messages=[
                types.PromptMessage(
                    role="user",
                    content=types.TextContent(
                        type="text",
                        text=(
                            f"Please inspect the mounted disk volumes:\n"
                            "1. Call `list_volumes` to see what is mounted\n"
                            f"2. Call `read_volume` with slot='{slot}' to dump the full "
                            "ProDOS filesystem tree including all file contents\n"
                            "3. Summarise the volume: name, total blocks, directory structure, "
                            "and any files that look relevant to debugging"
                        ),
                    ),
                )
            ],
        )

    elif name == "watch_execution":
        return types.GetPromptResult(
            description="Halt and inspect current execution point",
            messages=[
                types.PromptMessage(
                    role="user",
                    content=types.TextContent(
                        type="text",
                        text=(
                            "Please halt the emulator and show me where it is:\n"
                            "1. Call `halt_emulator`\n"
                            "2. Call `get_registers` to show the current CPU state\n"
                            "3. Call `debugger_command` with 'l BB/OOOO' to disassemble "
                            "20 instructions starting at the current PC\n"
                            "4. Ask me whether to step one instruction, continue running, "
                            "or set a breakpoint"
                        ),
                    ),
                )
            ],
        )

    raise ValueError(f"Unknown prompt: {name}")


@server.call_tool()
async def call_tool(name: str, arguments: dict[str, Any]) -> list[types.TextContent]:
    def text(s: str) -> list[types.TextContent]:
        return [types.TextContent(type="text", text=s)]

    if name == "get_registers":
        return text(_fmt(_send({"cmd": "get_registers"})))

    elif name == "read_memory":
        addr_raw = arguments["addr"]
        addr_int = _parse_addr(addr_raw)
        result = _send({"cmd": "read_memory",
                        "addr": addr_int,
                        "len": arguments.get("len", 256)})
        if result.get("ok") and "data_b64" in result:
            raw = _b64decode(result["data_b64"])
            result["hex_dump"] = _hex_dump(raw, addr_int)
        return text(_fmt(result))

    elif name == "search_memory":
        return text(_fmt(_send({
            "cmd": "search_memory",
            "start": _parse_addr(arguments["start"]),
            "end":   _parse_addr(arguments["end"]),
            "pattern": arguments["pattern"],
        })))

    elif name == "halt_emulator":
        return text(_fmt(_send({"cmd": "halt"})))

    elif name == "continue_emulator":
        return text(_fmt(_send({"cmd": "debugger_command", "text": "g"})))

    elif name == "step_emulator":
        return text(_fmt(_send({"cmd": "debugger_command", "text": "s"})))

    elif name == "get_break_info":
        return text(_fmt(_send({"cmd": "get_break_info"})))

    elif name == "debugger_command":
        return text(_fmt(_send({"cmd": "debugger_command",
                                "text": arguments["text"]})))

    elif name == "list_volumes":
        return text(_fmt(_send({"cmd": "list_volumes"})))

    elif name == "list_files":
        return text(_fmt(_send({
            "cmd": "list_files",
            "slot": arguments["slot"],
            "path": arguments.get("path", "/"),
            "recursive": 1 if arguments.get("recursive", False) else 0,
        })))

    elif name == "read_file":
        result = _send({"cmd": "read_file",
                        "slot": arguments["slot"],
                        "path": arguments["path"]})
        # Decode text preview for TXT ($04) and SRC ($B0) files
        if result.get("ok") and "data_b64" in result:
            ftype_hex = result.get("ftype", "")
            if ftype_hex in ("$04", "$B0"):
                raw = _b64decode(result["data_b64"])
                preview = raw.decode("latin-1", errors="replace").replace("\r", "\n")
                result["text_preview"] = preview[:4000]
        return text(_fmt(result))

    elif name == "read_volume_raw":
        result = _send({"cmd": "read_volume_raw", "slot": arguments["slot"]})
        return text(_fmt(result))

    elif name == "read_volume":
        result = _send({"cmd": "read_volume", "slot": arguments["slot"]})
        return text(_fmt(result))

    elif name == "restart_emulator":
        result = _send({"cmd": "restart"})
        return text(_fmt(result))

    elif name == "cold_reset":
        result = _send({"cmd": "debugger_command", "text": "C"})
        return text(_fmt(result))

    elif name == "get_log":
        result = _send({"cmd": "get_log", "lines": arguments.get("lines", 50)})
        if result.get("ok") and "log" in result:
            return text(result["log"])
        return text(_fmt(result))

    elif name == "write_memory":
        addr_int = _parse_addr(arguments["addr"])
        raw_bytes = [int(b, 16) for b in arguments["data"].split()]
        errors = []
        for i, byte_val in enumerate(raw_bytes):
            a = addr_int + i
            bank   = (a >> 16) & 0xFF
            offset = a & 0xFFFF
            cmd_str = f"{bank:02x}/{offset:04x}:{byte_val:02x}"
            r = _send({"cmd": "debugger_command", "text": cmd_str})
            if not r.get("ok"):
                errors.append(f"offset {i}: {r.get('error', '?')}")
        if errors:
            return text(json.dumps({"ok": False, "errors": errors,
                                    "bytes_written": len(raw_bytes) - len(errors)}))
        return text(json.dumps({"ok": True, "bytes_written": len(raw_bytes)}))

    elif name == "run_until":
        addr_int      = _parse_addr(arguments["addr"])
        timeout_s     = float(arguments.get("timeout_s", 60))
        poll_interval = max(1.0, float(arguments.get("poll_interval_s", 3.0)))
        bank   = (addr_int >> 16) & 0xFF
        offset = addr_int & 0xFFFF
        bp_str = f"{bank:02x}/{offset:04x}B"
        del_str = f"{bank:02x}/{offset:04x}D"

        # Set breakpoint
        _send({"cmd": "debugger_command", "text": bp_str})
        # Resume via main-thread debugger path to avoid event queue corruption
        _send({"cmd": "debugger_command", "text": "g"})

        hit = False
        deadline = time.time() + timeout_s
        regs = {}
        while time.time() < deadline:
            time.sleep(poll_interval)
            regs = _send({"cmd": "get_registers"})
            if regs.get("halted"):
                hit = True
                break

        # Always clean up the breakpoint
        _send({"cmd": "debugger_command", "text": del_str})

        regs["hit"]       = hit
        regs["timed_out"] = not hit
        return text(_fmt(regs))

    elif name == "get_screen_text":
        # ------------------------------------------------------------------
        # Auto-detect column mode and active page from emulator soft switches
        # unless the caller explicitly overrides them.
        #
        # Soft-switch status reads (Apple IIgs):
        #   $C01F  bit 7 = 1  → 80-column video enabled  (VID80)
        #   $C01C  bit 7 = 1  → text page 2 active        (PAGE2)
        #
        # These are read-only status registers — no side effects.
        # ------------------------------------------------------------------
        col80_arg = arguments.get("col80", None)   # None = auto-detect
        page_arg  = arguments.get("page",  None)   # None = auto-detect

        def _read1(addr):
            """Read a single byte via the debug socket; return (int|None)."""
            r = _send({"cmd": "read_memory", "addr": addr, "len": 1})
            if r.get("ok"):
                data = _b64decode(r["data_b64"])
                if data:
                    return data[0]
            return None

        # Detect column mode
        if col80_arg is None:
            vid80_byte = _read1(0x00C01F)
            if vid80_byte is not None:
                col80        = bool(vid80_byte & 0x80)
                col80_source = "auto"
            else:
                col80        = True          # safe fallback for GS/OS
                col80_source = "fallback"
        else:
            col80        = bool(col80_arg)
            col80_source = "caller"

        # Detect active text page
        if page_arg is None:
            page2_byte = _read1(0x00C01C)
            if page2_byte is not None:
                page        = 2 if (page2_byte & 0x80) else 1
                page_source = "auto"
            else:
                page        = 1
                page_source = "fallback"
        else:
            page        = int(page_arg)
            page_source = "caller"

        width = 80 if col80 else 40

        # Shadow RAM base addresses for text pages
        # Page 1: main=$E10400, aux=$E00400
        # Page 2: main=$E10800, aux=$E00800
        if page == 2:
            main_base = 0xE10800
            aux_base  = 0xE00800
        else:
            main_base = 0xE10400
            aux_base  = 0xE00400

        # Read the full 1 KB text page from shadow RAM in one call each
        main_r = _send({"cmd": "read_memory", "addr": main_base, "len": 0x400})
        if not main_r.get("ok"):
            return text(_fmt(main_r))
        main_raw = _b64decode(main_r["data_b64"])

        if col80:
            aux_r = _send({"cmd": "read_memory", "addr": aux_base, "len": 0x400})
            if not aux_r.get("ok"):
                return text(_fmt(aux_r))
            aux_raw = _b64decode(aux_r["data_b64"])

        def decode_char(byte_val):
            """Decode one Apple II text-screen byte to a printable character.

            Apple II text byte encoding:
              bit 7 = 1              Normal character   (most common)
              bit 7 = 0, bit 6 = 0  Inverse character  (0x00–0x3F in RAM)
              bit 7 = 0, bit 6 = 1  MouseText / Flash  (0x40–0x7F in RAM)

            After stripping the display-attribute bit (bit 7), the remaining
            7 bits select the character.  Values 0x20–0x7E map 1:1 to ASCII.
            Control codes (0x00–0x1F) and DEL (0x7F) are rendered as a space
            so they don't corrupt multi-line output but their position is
            preserved in the fixed-width line.
            """
            c = byte_val & 0x7F          # strip display-attribute bit
            if 0x20 <= c <= 0x7E:
                return chr(c)            # direct ASCII match
            # 0x00–0x1F: control / inverse control  → space placeholder
            # 0x7F:      DEL / checkerboard pattern → space placeholder
            return ' '

        # Build exactly 24 lines, each exactly `width` characters wide.
        # Apple II text-screen row addressing uses a non-linear interleave:
        #   row_offset = (row % 8) * 0x80 + (row // 8) * 0x28
        lines = []
        for row in range(24):
            row_off = (row % 8) * 0x80 + (row // 8) * 0x28
            chars = []
            if col80:
                # 80-col interleave: $E10400 ("main_raw") supplies even display
                # columns (0, 2, 4…) and $E00400 ("aux_raw") supplies odd display
                # columns (1, 3, 5…).  The variable names follow the shadow-RAM
                # convention in the original code, but empirically $E10400 holds
                # the first character of each pair — so it goes first.
                for col in range(40):
                    idx    = row_off + col
                    a_byte = aux_raw[idx]  if idx < len(aux_raw)  else 0xA0
                    m_byte = main_raw[idx] if idx < len(main_raw) else 0xA0
                    chars.append(decode_char(m_byte))
                    chars.append(decode_char(a_byte))
            else:
                for col in range(40):
                    idx    = row_off + col
                    m_byte = main_raw[idx] if idx < len(main_raw) else 0xA0
                    chars.append(decode_char(m_byte))

            # Pad or truncate to exact width so every line is identical length
            # and callers can lay them out like the real screen without guessing.
            line = "".join(chars)
            line = line.ljust(width)[:width]
            lines.append(line)

        meta = {
            "mode":         "80col" if col80 else "40col",
            "width":        width,
            "height":       24,
            "page":         page,
            "col80_source": col80_source,   # "auto" | "caller" | "fallback"
            "page_source":  page_source,    # "auto" | "caller" | "fallback"
        }
        raw_text = "\n".join(lines)
        return text(json.dumps(
            {"ok": True, "meta": meta, "lines": lines, "raw": raw_text},
            indent=2,
        ))

    elif name == "relaunch_app":
        wait_s = float(arguments.get("wait_s", 20))
        app_path = os.path.normpath(
            os.path.join(os.path.dirname(os.path.abspath(__file__)),
                         "..", "gsplus", "GSplus.app")
        )
        # Kill existing instance
        subprocess.run(["pkill", "-x", "GSplus"], capture_output=True)
        time.sleep(1.5)
        # Remove stale socket
        if os.path.exists(SOCK_PATH):
            try:
                os.unlink(SOCK_PATH)
            except OSError:
                pass
        # Relaunch
        try:
            subprocess.Popen(["open", app_path])
        except Exception as exc:
            return text(json.dumps({"ok": False,
                                    "error": f"Failed to launch {app_path}: {exc}"}))
        # Wait for socket
        deadline = time.time() + wait_s
        while time.time() < deadline:
            time.sleep(0.5)
            if os.path.exists(SOCK_PATH):
                time.sleep(1.0)   # Let the server finish binding
                return text(json.dumps({"ok": True,
                                        "message": "GSplus relaunched; socket ready."}))
        return text(json.dumps({"ok": False,
                                "error": f"Socket did not appear within {wait_s}s"}))

    elif name == "set_wdm_trap":
        trap_raw = arguments["trap"]
        enabled  = arguments.get("enabled", True)
        # Normalise trap to a JSON-serialisable form the C server understands.
        # MCP may deliver a list as the string "[1, 2, 5]" or "1, 2, 5".
        if isinstance(trap_raw, list):
            payload = trap_raw
        elif isinstance(trap_raw, int):
            payload = trap_raw
        elif isinstance(trap_raw, str):
            s = trap_raw.strip().strip("[]")
            if s.lower() == "all":
                payload = "all"
            elif "," in s:
                payload = [int(x.strip()) for x in s.split(",") if x.strip()]
            else:
                payload = int(s)   # single number as string
        else:
            payload = int(trap_raw)
        return text(_fmt(_send({
            "cmd":     "set_wdm_trap",
            "trap":    payload,
            "enabled": enabled,
        })))

    elif name == "get_wdm_traps":
        return text(_fmt(_send({"cmd": "get_wdm_traps"})))

    elif name == "send_keys":
        raw = arguments["keys"]
        # Interpret C-style escape sequences that arrive as literal characters
        # from the MCP client (e.g. backslash + 'n' → newline byte).
        import re
        def _unescape_keys(s: str) -> str:
            simple = {"\\n": "\n", "\\r": "\r", "\\t": "\t",
                      "\\a": "\a", "\\b": "\b", "\\0": "\0",
                      "\\\\": "\\"}
            def repl(m):
                seq = m.group(0)
                if seq in simple:
                    return simple[seq]
                if seq.startswith("\\x") and len(seq) == 4:
                    return chr(int(seq[2:], 16))
                return seq  # leave unrecognised escapes as-is
            return re.sub(r"\\x[0-9a-fA-F]{2}|\\[nrtab0\\]", repl, s)
        keys = _unescape_keys(raw)
        return text(_fmt(_send({"cmd": "send_keys",
                                "keys": keys})))

    else:
        return text(json.dumps({"error": f"unknown tool: {name}"}))


async def main() -> None:
    async with stdio_server() as (read_stream, write_stream):
        await server.run(
            read_stream,
            write_stream,
            server.create_initialization_options(),
        )


if __name__ == "__main__":
    asyncio.run(main())
