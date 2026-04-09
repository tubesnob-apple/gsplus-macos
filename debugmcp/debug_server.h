/*
 * debug_server.h — GSplus Apple IIgs emulator debug endpoint
 *
 * Exposes a Unix domain socket at /tmp/gsplus_debug.sock.
 * Protocol: newline-delimited JSON (one request per connection).
 *
 * Include in emulator C files that need to call these three functions.
 */

#ifndef DEBUG_SERVER_H
#define DEBUG_SERVER_H

/* Start the background socket server thread. Call once at emulator startup,
 * after debugger_init(). */
void debug_server_init(void);

/* Process any pending main-thread commands (debugger_command).
 * Call from the emulator 16ms loop after debugger_run_16ms(). */
void debug_server_poll(void);

/* Record CPU state at a break/halt so get_break_info can return it.
 * reason: 0=BRK instruction, 1=breakpoint, 2=halt_printf/fatal, 3=explicit */
void debug_server_notify_break(word32 pc, word32 acc, word32 xreg, word32 yreg,
                                word32 stack, word32 direct, word32 dbank,
                                word32 psr, int reason);

/* Append emulator diagnostic output to the persistent log ring buffer.
 * Called from debugger.c's dbg_vprintf on every dbg_printf invocation. */
void debug_server_log(const char *text, int len);

#endif /* DEBUG_SERVER_H */
