/* settings_bridge.c — see settings_bridge.h for the contract. */

#include "settings_bridge.h"
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern Cfg_menu g_cfg_main_menu[];
extern int      g_config_kegs_auto_update;
extern int      g_config_kegs_update_needed;
extern char     g_config_kegs_name[];

/* ── Submenu enumeration ─────────────────────────────────────────────
 *
 * The main menu is always submenu 0. Everything else is discovered by
 * walking g_cfg_main_menu and collecting CFGTYPE_MENU entries whose ptr
 * does not point back at the main menu itself (those are section titles,
 * not navigation links). */

#define MAX_SUBMENUS 32
static Cfg_menu *s_submenus[MAX_SUBMENUS];
static int       s_submenu_count = 0;
static int       s_submenus_built = 0;

static void
build_submenu_list(void)
{
	int i;

	s_submenu_count = 0;
	s_submenus[s_submenu_count++] = g_cfg_main_menu;

	for(i = 1; g_cfg_main_menu[i].str; i++) {
		Cfg_menu *e = &g_cfg_main_menu[i];
		Cfg_menu *target;
		if((e->cfgtype & 0xf) != CFGTYPE_MENU) continue;
		target = (Cfg_menu *)e->ptr;
		if(!target || target == g_cfg_main_menu) continue;
		if(s_submenu_count >= MAX_SUBMENUS) break;
		s_submenus[s_submenu_count++] = target;
	}
	s_submenus_built = 1;
}

int
settings_ui_submenu_count(void)
{
	if(!s_submenus_built) build_submenu_list();
	return s_submenu_count;
}

const char *
settings_ui_submenu_title(int idx)
{
	if(!s_submenus_built) build_submenu_list();
	if(idx < 0 || idx >= s_submenu_count) return "";
	return s_submenus[idx][0].str ? s_submenus[idx][0].str : "";
}

Cfg_menu *
settings_ui_submenu_entries(int idx)
{
	if(!s_submenus_built) build_submenu_list();
	if(idx < 0 || idx >= s_submenu_count) return NULL;
	return s_submenus[idx];
}

/* ── Per-entry introspection ─────────────────────────────────────── */

const char *
settings_ui_entry_str(const Cfg_menu *e)
{
	if(!e || !e->str) return "";
	return e->str;
}

int
settings_ui_entry_type(const Cfg_menu *e)
{
	if(!e) return 0;
	return e->cfgtype & 0xf;
}

int
settings_ui_entry_raw_type(const Cfg_menu *e)
{
	if(!e) return 0;
	return e->cfgtype;
}

int
settings_ui_entry_label(const Cfg_menu *e, char *outbuf, int maxlen)
{
	int n = 0;
	const char *p;

	if(!outbuf || maxlen <= 0) return 0;
	outbuf[0] = 0;
	if(!e || !e->str) return 0;
	for(p = e->str; *p && *p != ','; p++) {
		if(n < maxlen - 1) outbuf[n++] = *p;
	}
	while(n > 0 && (outbuf[n - 1] == ' ' || outbuf[n - 1] == '\t')) n--;
	outbuf[n] = 0;
	return n;
}

int
settings_ui_entry_has_options(const Cfg_menu *e)
{
	if(!e || !e->str) return 0;
	return strchr(e->str, ',') != NULL;
}

int
settings_ui_entry_options(const Cfg_menu *e, int *out_values, char *label_buf,
                          int label_stride, int max)
{
	const char *p;
	int count = 0;
	int reading_value = 1;
	char valbuf[64];
	int vlen = 0, llen = 0;
	char *cur_label;

	if(!e || !e->str || !label_buf || label_stride <= 1) return 0;

	p = strchr(e->str, ',');
	if(!p) return 0;
	p++;

	while(count < max) {
		if(reading_value) {
			vlen = 0;
			while(*p && *p != ',') {
				if(vlen < (int)sizeof(valbuf) - 1)
					valbuf[vlen++] = *p;
				p++;
			}
			valbuf[vlen] = 0;
			if(vlen == 0) break;
			if(out_values)
				out_values[count] = (int)strtol(valbuf, NULL, 0);
			if(*p == ',') p++;
			reading_value = 0;
		} else {
			llen = 0;
			cur_label = label_buf + count * label_stride;
			while(*p && *p != ',') {
				if(llen < label_stride - 1)
					cur_label[llen++] = *p;
				p++;
			}
			/* Trim leading/trailing spaces */
			cur_label[llen] = 0;
			while(llen > 0 && cur_label[llen - 1] == ' ') {
				cur_label[--llen] = 0;
			}
			{
				int lead = 0;
				while(cur_label[lead] == ' ') lead++;
				if(lead > 0) {
					memmove(cur_label, cur_label + lead,
					        llen - lead + 1);
				}
			}
			count++;
			if(*p == ',') p++;
			reading_value = 1;
		}
	}
	return count;
}

/* ── Typed getters / setters ─────────────────────────────────────── */

int
settings_ui_entry_get_int(const Cfg_menu *e)
{
	int *iptr;
	if(!e || (e->cfgtype & 0xf) != CFGTYPE_INT) return 0;
	iptr = (int *)e->ptr;
	if(!iptr) return 0;
	return *iptr;
}

void
settings_ui_entry_set_int(const Cfg_menu *e, int value)
{
	int *iptr;
	if(!e || (e->cfgtype & 0xf) != CFGTYPE_INT) return;
	iptr = (int *)e->ptr;
	if(!iptr) return;
	cfg_int_update(iptr, value);
}

const char *
settings_ui_entry_get_str(const Cfg_menu *e)
{
	char **sptr;
	int t;
	if(!e) return "";
	t = e->cfgtype & 0xf;
	if(t != CFGTYPE_FILE && t != CFGTYPE_STR && t != CFGTYPE_DIR)
		return "";
	sptr = (char **)e->ptr;
	if(!sptr || !*sptr) return "";
	return *sptr;
}

void
settings_ui_entry_set_str(const Cfg_menu *e, const char *value)
{
	char **sptr;
	int t;
	if(!e || !value) return;
	t = e->cfgtype & 0xf;
	if(t != CFGTYPE_FILE && t != CFGTYPE_STR && t != CFGTYPE_DIR) return;
	sptr = (char **)e->ptr;
	if(!sptr) return;
	cfg_file_update_ptr(sptr, value, 1);
}

/* ── Disk helpers ────────────────────────────────────────────────── */

int
settings_ui_entry_disk_code(const Cfg_menu *e)
{
	if(!e || (e->cfgtype & 0xf) != CFGTYPE_DISK) return -1;
	return (e->cfgtype >> 4) & 0xfff;
}

int
settings_ui_entry_disk_name(const Cfg_menu *e, char *outbuf, int maxlen)
{
	int type_ext;
	if(!outbuf || maxlen <= 0) return 0;
	outbuf[0] = 0;
	type_ext = settings_ui_entry_disk_code(e);
	if(type_ext < 0) return 0;
	return cfg_get_disk_name(outbuf, maxlen, type_ext, 0);
}

int
settings_ui_entry_disk_locked(const Cfg_menu *e)
{
	int type_ext = settings_ui_entry_disk_code(e);
	if(type_ext < 0) return 0;
	return cfg_get_disk_locked(type_ext);
}

int
settings_ui_disk_mount(int type_ext, const char *path)
{
	int slot, drive, ret;
	if(!path || !*path) return 0;
	slot = (type_ext >> 8) & 0xf;
	drive = type_ext & 0xff;
	ret = cfg_maybe_insert_disk(slot, drive, path);
	return ret > 0 ? 1 : 0;
}

void
settings_ui_disk_eject(int type_ext)
{
	iwm_eject_disk_by_num((type_ext >> 8) & 0xf, type_ext & 0xff);
	g_config_kegs_update_needed = 1;
}

/* ── CFGTYPE_FUNC ─────────────────────────────────────────────────── */

void
settings_ui_entry_invoke_func(const Cfg_menu *e)
{
	char *(*fn_ptr)(int);
	if(!e || (e->cfgtype & 0xf) != CFGTYPE_FUNC) return;
	fn_ptr = (char *(*)(int))e->ptr;
	if(!fn_ptr) return;
	(void)(*fn_ptr)(0);
}

/* ── Global actions ──────────────────────────────────────────────── */

int
settings_ui_auto_update_enabled(void)
{
	return g_config_kegs_auto_update;
}

int
settings_ui_save_now(void)
{
	/* config_write_config_kegs_file returns NULL on success, non-NULL
	 * pointer to a status string on failure. */
	return config_write_config_kegs_file(0) == NULL ? 0 : -1;
}

const char *
settings_ui_config_path(void)
{
	return &g_config_kegs_name[0];
}
