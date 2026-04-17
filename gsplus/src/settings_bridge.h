/* settings_bridge.h — C API exposing the kegs config menu tree to the
 * native macOS preferences window.
 *
 * The existing Cfg_menu arrays in config.c are the single source of truth
 * for what's configurable. This bridge lets Swift enumerate them, read
 * and write the underlying globals through the same cfg_*_update helpers
 * the text UI uses, and invoke CFGTYPE_FUNC actions. All mutations flow
 * through the same path as the F4 menu, so the config.kegs auto-update
 * and write behaviour is identical.
 */

#ifndef GSPLUS_SETTINGS_BRIDGE_H
#define GSPLUS_SETTINGS_BRIDGE_H

#include "defc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── Submenu enumeration ────────────────────────────────────────────────
 * Submenus are the top-level sections shown as tabs. Index 0 is always
 * the main menu. Subsequent indexes correspond to each CFGTYPE_MENU entry
 * inside the main menu, in order. */
int              settings_ui_submenu_count(void);
const char      *settings_ui_submenu_title(int idx);
Cfg_menu        *settings_ui_submenu_entries(int idx);

/* Given the entry pointer returned by settings_ui_submenu_entries(),
 * iterate with entry++. A zeroed entry (str == NULL) terminates. */

/* ── Per-entry introspection ───────────────────────────────────────── */

/* Full string from Cfg_menu.str. First comma-separated token is the label;
 * subsequent pairs (value,label) form the enum options. */
const char      *settings_ui_entry_str(const Cfg_menu *e);

/* Type masked to the low 4 bits (CFGTYPE_MENU/INT/DISK/FUNC/FILE/STR/DIR). */
int              settings_ui_entry_type(const Cfg_menu *e);

/* Raw cfgtype field (includes slot/drive encoding for CFGTYPE_DISK). */
int              settings_ui_entry_raw_type(const Cfg_menu *e);

/* Copies the display label (up to first comma, trimmed) into outbuf.
 * Returns strlen written. */
int              settings_ui_entry_label(const Cfg_menu *e,
                                         char *outbuf, int maxlen);

/* True if the entry has a comma-separated enum option list. */
int              settings_ui_entry_has_options(const Cfg_menu *e);

/* Writes up to max options into out_values/out_labels (labels into a
 * caller-provided label_buf, entries are laid out one per slot). Returns
 * count of options actually written. Each label slot is label_buf +
 * i*label_stride. */
int              settings_ui_entry_options(const Cfg_menu *e,
                                           int *out_values,
                                           char *label_buf,
                                           int label_stride,
                                           int max);

/* ── Typed getters / setters (CFGTYPE_INT / CFGTYPE_FILE /
 *    CFGTYPE_STR / CFGTYPE_DIR) ─────────────────────────────────────── */

int              settings_ui_entry_get_int(const Cfg_menu *e);
void             settings_ui_entry_set_int(const Cfg_menu *e, int value);

/* Returns pointer to the current string (borrow, do not free).
 * Returns "" if no value is set. */
const char      *settings_ui_entry_get_str(const Cfg_menu *e);
void             settings_ui_entry_set_str(const Cfg_menu *e,
                                           const char *value);

/* ── CFGTYPE_DISK helpers ──────────────────────────────────────────── */

/* Returns the (slot << 8) | drive code encoded in the raw cfgtype, or -1
 * if the entry is not a disk. */
int              settings_ui_entry_disk_code(const Cfg_menu *e);

/* Copies the mounted image path into outbuf (empty string if unmounted).
 * Returns the dynapro_blocks value (>0 if a dynapro directory). */
int              settings_ui_entry_disk_name(const Cfg_menu *e,
                                             char *outbuf, int maxlen);

/* Lock state: 0 = unlocked, 1 = hardware write-protect, 2 = not
 * write-through. Mirrors cfg_get_disk_locked(). */
int              settings_ui_entry_disk_locked(const Cfg_menu *e);

/* Mount / eject a disk image. type_ext is (slot << 8) | drive; use
 * settings_ui_entry_disk_code() to get it from an entry. Returns 1 on
 * success, 0 on failure (errors also print to stderr/debug log). */
int              settings_ui_disk_mount(int type_ext, const char *path);
void             settings_ui_disk_eject(int type_ext);

/* ── CFGTYPE_FUNC ──────────────────────────────────────────────────── */

void             settings_ui_entry_invoke_func(const Cfg_menu *e);

/* ── Global actions ────────────────────────────────────────────────── */

/* Whether config.kegs is being auto-flushed every 16ms. */
int              settings_ui_auto_update_enabled(void);

/* Flush config.kegs to disk immediately. Returns 0 on success. */
int              settings_ui_save_now(void);

/* Path of the active config.kegs file (borrowed, do not free). */
const char      *settings_ui_config_path(void);

#ifdef __cplusplus
}
#endif

#endif
