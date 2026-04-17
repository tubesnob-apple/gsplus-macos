/**********************************************************************/
/*                    GSplus - Apple //gs Emulator                    */
/*                    Copyright 2025-2026 GSplus Contributors         */
/*                                                                    */
/*      This code is covered by the GNU GPL v3                        */
/*      See the file COPYING.txt or https://www.gnu.org/licenses/     */
/**********************************************************************/

import Cocoa

// CFGTYPE_* constants — mirror values from config.h (Swift can't see #defines).
fileprivate let CFGTYPE_MENU: Int32 = 1
fileprivate let CFGTYPE_INT:  Int32 = 2
fileprivate let CFGTYPE_DISK: Int32 = 3
fileprivate let CFGTYPE_FUNC: Int32 = 4
fileprivate let CFGTYPE_FILE: Int32 = 5
fileprivate let CFGTYPE_STR:  Int32 = 6
fileprivate let CFGTYPE_DIR:  Int32 = 7

// Native macOS replacement for the F4 text-based config screen.
//
// The C side (settings_bridge.c) walks g_cfg_main_menu and exposes each
// submenu + entry. This file reflects that tree as an NSTabView with one
// tab per submenu; inside each tab, rows are built by type:
//
//   CFGTYPE_INT  (with enum options) → NSPopUpButton
//   CFGTYPE_INT  (bare)              → NSTextField (numeric, updates on edit-end)
//   CFGTYPE_STR                      → NSTextField
//   CFGTYPE_FILE                     → path label + "Choose…" (NSOpenPanel files)
//   CFGTYPE_DIR                      → path label + "Choose…" (NSOpenPanel folders)
//   CFGTYPE_DISK                     → filename label + Mount / Eject buttons
//   CFGTYPE_FUNC                     → NSButton
//   CFGTYPE_MENU                     → skipped (those are tab nav; rendered
//                                       implicitly by each submenu being its
//                                       own tab)
//
// All writes go through settings_ui_entry_set_*, which call cfg_int_update
// or cfg_file_update_ptr — the same path the F4 menu uses — so config.kegs
// auto-save and side effects (ROM reload, SCC reconfig, symbols rescan…)
// all just work.

class SettingsWindowController {

    private var window:   NSWindow?
    private var tabView:  NSTabView!

    /// Holds per-row UI references so we can refresh when the underlying
    /// C globals change (e.g., after selecting a file via the NSOpenPanel).
    private var rowUpdaters: [() -> Void] = []

    var isOpen: Bool { return window?.isVisible ?? false }

    // MARK: - Public API

    func show() {
        if window == nil { build() }
        refreshAll()
        window?.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
    }

    func toggle() {
        if isOpen { window?.orderOut(nil) } else { show() }
    }

    // MARK: - Construction

    private func build() {
        let rect = NSRect(x: 160, y: 160, width: 780, height: 560)
        let style: NSWindow.StyleMask = [.titled, .closable,
                                          .resizable, .miniaturizable]
        let w = NSWindow(contentRect: rect, styleMask: style,
                         backing: .buffered, defer: false)
        w.title              = "GSplus Preferences"
        w.isReleasedWhenClosed = false

        guard let content = w.contentView else { return }

        // Bottom footer (Save / Close)
        let footer = NSView(frame: NSRect(x: 0, y: 0,
                                          width: rect.width, height: 44))
        footer.autoresizingMask = [.width, .minYMargin]

        let saveBtn = NSButton(title: "Save config.kegs now",
                               target: self,
                               action: #selector(onSaveNow(_:)))
        saveBtn.bezelStyle = .rounded
        saveBtn.sizeToFit()
        var sf = saveBtn.frame
        sf.origin = NSPoint(x: 16, y: 8)
        saveBtn.frame = sf
        footer.addSubview(saveBtn)

        let closeBtn = NSButton(title: "Close",
                                target: self,
                                action: #selector(onClose(_:)))
        closeBtn.bezelStyle = .rounded
        closeBtn.keyEquivalent = "\r"
        closeBtn.sizeToFit()
        var cf = closeBtn.frame
        cf.origin = NSPoint(x: rect.width - cf.width - 16, y: 8)
        cf.size.width = max(cf.width, 90)
        closeBtn.autoresizingMask = [.minXMargin]
        closeBtn.frame = cf
        footer.addSubview(closeBtn)

        content.addSubview(footer)

        // NSTabView filling the rest
        let tv = NSTabView(frame: NSRect(x: 0, y: 44,
                                         width: rect.width,
                                         height: rect.height - 44))
        tv.autoresizingMask = [.width, .height]
        content.addSubview(tv)
        tabView = tv

        populateTabs()
        window = w
    }

    private func populateTabs() {
        let count = Int(settings_ui_submenu_count())
        for i in 0 ..< count {
            let titlePtr = settings_ui_submenu_title(Int32(i))
            let title    = titlePtr.flatMap { String(cString: $0) } ?? "Section \(i)"
            let entries  = settings_ui_submenu_entries(Int32(i))

            let item = NSTabViewItem(identifier: "tab\(i)")
            item.label = shortenedTitle(title)
            item.view  = buildSubmenuView(entries: entries, title: title)
            tabView.addTabViewItem(item)
        }
    }

    /// Strip the redundant "Configuration" / "Config" suffix so tabs read
    /// "Disk", "Joystick", "ROM File", "Video", etc.
    private func shortenedTitle(_ s: String) -> String {
        var t = s
        for suffix in [" Configuration", " Settings", " File Selection",
                       " Selection"] {
            if t.hasSuffix(suffix) {
                t = String(t.dropLast(suffix.count))
                break
            }
        }
        // Special cases
        if t == "KEGS" { return "General" }
        return t.trimmingCharacters(in: .whitespaces)
    }

    // MARK: - Submenu view (one per tab)

    private func buildSubmenuView(entries: UnsafeMutablePointer<Cfg_menu>?,
                                  title: String) -> NSView {
        let scroll = NSScrollView(frame: NSRect(x: 0, y: 0, width: 700,
                                                height: 500))
        scroll.autoresizingMask        = [.width, .height]
        scroll.hasVerticalScroller     = true
        scroll.hasHorizontalScroller   = false
        scroll.drawsBackground         = false
        scroll.borderType              = .noBorder

        let doc = NSFlippedContentView(frame:
            NSRect(x: 0, y: 0, width: 700, height: 10))
        doc.autoresizingMask = [.width]

        var y: CGFloat = 12
        let rowH: CGFloat = 28
        let gapH: CGFloat = 6
        let sectionGap: CGFloat = 12

        guard let entries = entries else {
            let lbl = NSTextField(labelWithString:
                "No entries for \(title)")
            lbl.frame = NSRect(x: 16, y: y, width: 660, height: rowH)
            doc.addSubview(lbl)
            doc.frame = NSRect(x: 0, y: 0, width: 700, height: y + rowH + 12)
            scroll.documentView = doc
            return scroll
        }

        var i = 0
        // Skip entry 0 — it's the self-reference title used by the TUI
        // as the header for the menu; we use our tab label instead.
        i = 1
        while true {
            var e = entries[i]
            guard e.str != nil else { break }
            let cfgtype = Int(settings_ui_entry_type(&e))
            let labelStr = cString64(capacity: 512) { buf, cap in
                settings_ui_entry_label(&e, buf, cap)
            }

            let entryPtr = entries.advanced(by: i)

            // Empty string with no type == spacer
            if labelStr.isEmpty && cfgtype == 0 {
                y += sectionGap
                i += 1
                continue
            }

            // Section header (non-empty label, no ptr, no type)
            if cfgtype == 0 && e.ptr == nil {
                let hdr = NSTextField(labelWithString: labelStr)
                hdr.font = NSFont.boldSystemFont(ofSize: 13)
                hdr.frame = NSRect(x: 16, y: y, width: 660, height: rowH)
                doc.addSubview(hdr)
                y += rowH
                i += 1
                continue
            }

            // CFGTYPE_MENU: navigation to a submenu; the submenu gets its own
            // tab, so skip here to avoid duplicating the same options.
            if cfgtype == Int(CFGTYPE_MENU) {
                i += 1
                continue
            }

            addRow(to: doc, y: y, height: rowH, label: labelStr,
                   entryPtr: entryPtr, cfgtype: cfgtype)
            y += rowH + gapH
            i += 1
        }

        doc.frame = NSRect(x: 0, y: 0, width: 700, height: y + 12)
        scroll.documentView = doc
        return scroll
    }

    // MARK: - Row factories

    private func addRow(to parent: NSView, y: CGFloat, height: CGFloat,
                        label: String,
                        entryPtr: UnsafeMutablePointer<Cfg_menu>,
                        cfgtype: Int) {
        // Left-side label (for non-FUNC rows; FUNC rows are just a button)
        let labelW: CGFloat = 260
        let controlX: CGFloat = 16 + labelW + 8
        let controlW: CGFloat = 700 - controlX - 16

        if cfgtype != Int(CFGTYPE_FUNC) {
            let lbl = NSTextField(labelWithString: label + ":")
            lbl.frame = NSRect(x: 16, y: y + 4, width: labelW,
                               height: height - 4)
            lbl.alignment = .right
            lbl.textColor = .secondaryLabelColor
            parent.addSubview(lbl)
        }

        switch cfgtype {
        case Int(CFGTYPE_INT):
            if settings_ui_entry_has_options(entryPtr) != 0 {
                addIntEnumRow(parent: parent, x: controlX, y: y,
                              w: controlW, h: height, entryPtr: entryPtr)
            } else {
                addIntFieldRow(parent: parent, x: controlX, y: y,
                               w: controlW, h: height, entryPtr: entryPtr)
            }
        case Int(CFGTYPE_STR):
            addStringRow(parent: parent, x: controlX, y: y,
                         w: controlW, h: height, entryPtr: entryPtr)
        case Int(CFGTYPE_FILE):
            addPathRow(parent: parent, x: controlX, y: y,
                       w: controlW, h: height, entryPtr: entryPtr,
                       pickDir: false)
        case Int(CFGTYPE_DIR):
            addPathRow(parent: parent, x: controlX, y: y,
                       w: controlW, h: height, entryPtr: entryPtr,
                       pickDir: true)
        case Int(CFGTYPE_DISK):
            addDiskRow(parent: parent, x: controlX, y: y,
                       w: controlW, h: height, entryPtr: entryPtr,
                       labelText: label)
        case Int(CFGTYPE_FUNC):
            addFuncRow(parent: parent, y: y, h: height,
                       entryPtr: entryPtr, labelText: label)
        default:
            let note = NSTextField(labelWithString:
                "(unsupported type \(cfgtype))")
            note.frame = NSRect(x: controlX, y: y + 4,
                                width: controlW, height: height - 4)
            note.textColor = .systemRed
            parent.addSubview(note)
        }
    }

    private func addIntEnumRow(parent: NSView, x: CGFloat, y: CGFloat,
                               w: CGFloat, h: CGFloat,
                               entryPtr: UnsafeMutablePointer<Cfg_menu>) {
        let popup = NSPopUpButton(frame:
            NSRect(x: x, y: y + 2, width: min(w, 340), height: h - 4),
                                  pullsDown: false)

        // Read options
        let maxOpts = 32
        let stride  = 64
        var values  = [Int32](repeating: 0, count: maxOpts)
        let buf     = UnsafeMutablePointer<CChar>
                        .allocate(capacity: maxOpts * stride)
        defer { buf.deallocate() }
        buf.initialize(repeating: 0, count: maxOpts * stride)
        let n = Int(settings_ui_entry_options(entryPtr,
                        &values, buf, Int32(stride), Int32(maxOpts)))

        for i in 0 ..< n {
            let lbl = String(cString: buf.advanced(by: i * stride))
            popup.addItem(withTitle: lbl)
            popup.lastItem?.tag = Int(values[i])
        }

        popup.target = SettingsRowTarget.shared
        popup.action = #selector(SettingsRowTarget.onPopupChanged(_:))
        SettingsRowTarget.shared.register(popup: popup, entry: entryPtr)

        parent.addSubview(popup)

        rowUpdaters.append { [weak popup] in
            guard let popup = popup else { return }
            let cur = Int(settings_ui_entry_get_int(entryPtr))
            if let idx = popup.itemArray.firstIndex(where: { $0.tag == cur }) {
                popup.selectItem(at: idx)
            } else {
                popup.addItem(withTitle: "\(cur) (custom)")
                popup.lastItem?.tag = cur
                popup.selectItem(withTag: cur)
            }
        }
    }

    private func addIntFieldRow(parent: NSView, x: CGFloat, y: CGFloat,
                                w: CGFloat, h: CGFloat,
                                entryPtr: UnsafeMutablePointer<Cfg_menu>) {
        let tf = NSTextField(frame:
            NSRect(x: x, y: y + 2, width: 120, height: h - 4))
        tf.alignment = .right
        tf.target = SettingsRowTarget.shared
        tf.action = #selector(SettingsRowTarget.onIntFieldChanged(_:))
        SettingsRowTarget.shared.register(intField: tf, entry: entryPtr)
        parent.addSubview(tf)

        rowUpdaters.append { [weak tf] in
            guard let tf = tf else { return }
            tf.stringValue = String(Int(settings_ui_entry_get_int(entryPtr)))
        }
    }

    private func addStringRow(parent: NSView, x: CGFloat, y: CGFloat,
                              w: CGFloat, h: CGFloat,
                              entryPtr: UnsafeMutablePointer<Cfg_menu>) {
        let tf = NSTextField(frame:
            NSRect(x: x, y: y + 2, width: w, height: h - 4))
        tf.target = SettingsRowTarget.shared
        tf.action = #selector(SettingsRowTarget.onStrFieldChanged(_:))
        SettingsRowTarget.shared.register(strField: tf, entry: entryPtr)
        parent.addSubview(tf)

        rowUpdaters.append { [weak tf] in
            guard let tf = tf else { return }
            tf.stringValue = cString(settings_ui_entry_get_str(entryPtr))
        }
    }

    private func addPathRow(parent: NSView, x: CGFloat, y: CGFloat,
                            w: CGFloat, h: CGFloat,
                            entryPtr: UnsafeMutablePointer<Cfg_menu>,
                            pickDir: Bool) {
        let btnW: CGFloat = 110
        let gap: CGFloat = 8
        let pathW = w - btnW - gap

        let pathLbl = NSTextField(labelWithString: "")
        pathLbl.frame = NSRect(x: x, y: y + 4, width: pathW, height: h - 6)
        pathLbl.lineBreakMode = .byTruncatingHead
        pathLbl.textColor = .labelColor
        pathLbl.font = NSFont.systemFont(ofSize: 12)
        parent.addSubview(pathLbl)

        let sel: Selector = pickDir
            ? #selector(SettingsRowTarget.onPickDir(_:))
            : #selector(SettingsRowTarget.onPickFile(_:))
        let btn = NSButton(title: "Choose…",
                           target: SettingsRowTarget.shared,
                           action: sel)
        btn.bezelStyle = .rounded
        btn.frame = NSRect(x: x + pathW + gap, y: y + 2,
                           width: btnW, height: h - 4)
        SettingsRowTarget.shared.register(pathBtn: btn, entry: entryPtr,
                                          label: pathLbl, dir: pickDir,
                                          onChange: { [weak self] in
            self?.refreshAll()
        })
        parent.addSubview(btn)

        rowUpdaters.append { [weak pathLbl] in
            guard let lbl = pathLbl else { return }
            lbl.stringValue = cString(settings_ui_entry_get_str(entryPtr))
        }
    }

    private func addDiskRow(parent: NSView, x: CGFloat, y: CGFloat,
                            w: CGFloat, h: CGFloat,
                            entryPtr: UnsafeMutablePointer<Cfg_menu>,
                            labelText: String) {
        let ejectW: CGFloat = 70
        let mountW: CGFloat = 90
        let gap: CGFloat = 8
        let nameW = w - mountW - ejectW - 2 * gap

        let nameLbl = NSTextField(labelWithString: "")
        nameLbl.frame = NSRect(x: x, y: y + 4, width: nameW, height: h - 6)
        nameLbl.lineBreakMode = .byTruncatingHead
        nameLbl.font = NSFont.systemFont(ofSize: 12)
        parent.addSubview(nameLbl)

        let mountBtn = NSButton(title: "Mount…",
                                target: SettingsRowTarget.shared,
                                action: #selector(
                                    SettingsRowTarget.onDiskMount(_:)))
        mountBtn.bezelStyle = .rounded
        mountBtn.frame = NSRect(x: x + nameW + gap, y: y + 2,
                                width: mountW, height: h - 4)

        let ejectBtn = NSButton(title: "Eject",
                                target: SettingsRowTarget.shared,
                                action: #selector(
                                    SettingsRowTarget.onDiskEject(_:)))
        ejectBtn.bezelStyle = .rounded
        ejectBtn.frame = NSRect(x: x + nameW + mountW + 2 * gap, y: y + 2,
                                width: ejectW, height: h - 4)

        SettingsRowTarget.shared.register(diskRow:
            DiskRowRefs(mount: mountBtn, eject: ejectBtn, name: nameLbl,
                        entry: entryPtr,
                        onChange: { [weak self] in self?.refreshAll() }))
        parent.addSubview(mountBtn)
        parent.addSubview(ejectBtn)

        rowUpdaters.append { [weak nameLbl, weak ejectBtn] in
            let name = cString64(capacity: 1024) { buf, cap in
                settings_ui_entry_disk_name(entryPtr, buf, cap)
            }
            nameLbl?.stringValue = name.isEmpty ? "(empty)" : name
            ejectBtn?.isEnabled = !name.isEmpty
        }
    }

    private func addFuncRow(parent: NSView, y: CGFloat, h: CGFloat,
                            entryPtr: UnsafeMutablePointer<Cfg_menu>,
                            labelText: String) {
        let btn = NSButton(title: labelText,
                           target: SettingsRowTarget.shared,
                           action: #selector(
                               SettingsRowTarget.onFuncButton(_:)))
        btn.bezelStyle = .rounded
        btn.frame = NSRect(x: 16, y: y + 2, width: 300, height: h - 4)
        SettingsRowTarget.shared.register(funcBtn: btn, entry: entryPtr)
        parent.addSubview(btn)
    }

    // MARK: - Refresh

    private func refreshAll() {
        for update in rowUpdaters { update() }
    }

    // MARK: - Footer actions

    @objc private func onSaveNow(_ sender: Any?) {
        let rc = settings_ui_save_now()
        let path = cString(settings_ui_config_path())
        let alert = NSAlert()
        alert.messageText = rc == 0 ? "Saved" : "Save failed"
        alert.informativeText = rc == 0
            ? "Configuration written to \(path)"
            : "Could not write \(path). Check the console for errors."
        alert.alertStyle = rc == 0 ? .informational : .warning
        alert.runModal()
    }

    @objc private func onClose(_ sender: Any?) {
        window?.orderOut(nil)
    }
}

// ---------------------------------------------------------------------------
// MARK: - NSFlippedContentView
// ---------------------------------------------------------------------------

/// A flipped NSView so subview y-coordinates grow downward, matching how
/// we lay the settings rows out top-to-bottom.
fileprivate class NSFlippedContentView: NSView {
    override var isFlipped: Bool { return true }
}

// ---------------------------------------------------------------------------
// MARK: - Row target / action bridge
// ---------------------------------------------------------------------------

fileprivate struct DiskRowRefs {
    weak var mount:  NSButton?
    weak var eject:  NSButton?
    weak var name:   NSTextField?
    let entry:       UnsafeMutablePointer<Cfg_menu>
    let onChange:    () -> Void
}

/// NSControl actions want an @objc target. We route all settings-row
/// actions through a single shared object that dispatches based on the
/// sender's identity.
fileprivate class SettingsRowTarget: NSObject, NSTextFieldDelegate {
    static let shared = SettingsRowTarget()

    private var popupEntries:   [ObjectIdentifier: UnsafeMutablePointer<Cfg_menu>] = [:]
    private var intFieldEntries:[ObjectIdentifier: UnsafeMutablePointer<Cfg_menu>] = [:]
    private var strFieldEntries:[ObjectIdentifier: UnsafeMutablePointer<Cfg_menu>] = [:]
    private var funcEntries:    [ObjectIdentifier: UnsafeMutablePointer<Cfg_menu>] = [:]

    private struct PathRef {
        let entry: UnsafeMutablePointer<Cfg_menu>
        weak var label: NSTextField?
        let isDir: Bool
        let onChange: () -> Void
    }
    private var pathRefs: [ObjectIdentifier: PathRef] = [:]
    private var diskRefs: [ObjectIdentifier: DiskRowRefs] = [:]

    func register(popup: NSPopUpButton,
                  entry: UnsafeMutablePointer<Cfg_menu>) {
        popupEntries[ObjectIdentifier(popup)] = entry
    }
    func register(intField: NSTextField,
                  entry: UnsafeMutablePointer<Cfg_menu>) {
        intFieldEntries[ObjectIdentifier(intField)] = entry
    }
    func register(strField: NSTextField,
                  entry: UnsafeMutablePointer<Cfg_menu>) {
        strFieldEntries[ObjectIdentifier(strField)] = entry
    }
    func register(funcBtn: NSButton,
                   entry: UnsafeMutablePointer<Cfg_menu>) {
        funcEntries[ObjectIdentifier(funcBtn)] = entry
    }
    func register(pathBtn: NSButton,
                  entry: UnsafeMutablePointer<Cfg_menu>,
                  label: NSTextField,
                  dir: Bool,
                  onChange: @escaping () -> Void) {
        pathRefs[ObjectIdentifier(pathBtn)] =
            PathRef(entry: entry, label: label, isDir: dir,
                    onChange: onChange)
    }
    func register(diskRow refs: DiskRowRefs) {
        if let mb = refs.mount { diskRefs[ObjectIdentifier(mb)] = refs }
        if let eb = refs.eject { diskRefs[ObjectIdentifier(eb)] = refs }
    }

    @objc func onPopupChanged(_ sender: NSPopUpButton) {
        guard let entry = popupEntries[ObjectIdentifier(sender)] else { return }
        let tag = Int32(sender.selectedTag())
        settings_ui_entry_set_int(entry, tag)
    }

    @objc func onIntFieldChanged(_ sender: NSTextField) {
        guard let entry = intFieldEntries[ObjectIdentifier(sender)]
            else { return }
        let s = sender.stringValue.trimmingCharacters(in: .whitespaces)
        // Accept decimal, 0x hex, and $ hex
        let val: Int?
        if s.hasPrefix("$") {
            val = Int(s.dropFirst(), radix: 16)
        } else if s.hasPrefix("0x") || s.hasPrefix("0X") {
            val = Int(s.dropFirst(2), radix: 16)
        } else {
            val = Int(s)
        }
        if let v = val {
            settings_ui_entry_set_int(entry, Int32(v))
            sender.stringValue = String(
                Int(settings_ui_entry_get_int(entry)))
        } else {
            // Revert
            sender.stringValue = String(
                Int(settings_ui_entry_get_int(entry)))
            NSSound.beep()
        }
    }

    @objc func onStrFieldChanged(_ sender: NSTextField) {
        guard let entry = strFieldEntries[ObjectIdentifier(sender)]
            else { return }
        sender.stringValue.withCString { cptr in
            settings_ui_entry_set_str(entry, cptr)
        }
    }

    @objc func onFuncButton(_ sender: NSButton) {
        guard let entry = funcEntries[ObjectIdentifier(sender)]
            else { return }
        settings_ui_entry_invoke_func(entry)
    }

    @objc func onPickFile(_ sender: NSButton) {
        guard let ref = pathRefs[ObjectIdentifier(sender)] else { return }
        runOpenPanel(sender: sender, directory: false, ref: ref)
    }

    @objc func onPickDir(_ sender: NSButton) {
        guard let ref = pathRefs[ObjectIdentifier(sender)] else { return }
        runOpenPanel(sender: sender, directory: true, ref: ref)
    }

    private func runOpenPanel(sender: NSButton, directory: Bool,
                              ref: PathRef) {
        let panel = NSOpenPanel()
        panel.canChooseFiles        = !directory
        panel.canChooseDirectories  = directory
        panel.allowsMultipleSelection = false
        panel.resolvesAliases       = true
        if directory { panel.title = "Choose folder" }
        else         { panel.title = "Choose file" }
        // Start the panel in the current value's directory if present
        let cur = String(cString: settings_ui_entry_get_str(ref.entry))
        if !cur.isEmpty {
            let u = URL(fileURLWithPath: cur)
            panel.directoryURL = directory ? u : u.deletingLastPathComponent()
        }

        if let w = sender.window {
            panel.beginSheetModal(for: w) { resp in
                if resp == .OK, let u = panel.url {
                    u.path.withCString { cptr in
                        settings_ui_entry_set_str(ref.entry, cptr)
                    }
                    ref.onChange()
                }
            }
        } else {
            if panel.runModal() == .OK, let u = panel.url {
                u.path.withCString { cptr in
                    settings_ui_entry_set_str(ref.entry, cptr)
                }
                ref.onChange()
            }
        }
    }

    @objc func onDiskMount(_ sender: NSButton) {
        guard let refs = diskRefs[ObjectIdentifier(sender)] else { return }
        let panel = NSOpenPanel()
        panel.canChooseFiles = true
        panel.canChooseDirectories = true  // allow dynapro directories
        panel.allowsMultipleSelection = false
        panel.title = "Mount disk image"
        panel.message = "Pick a disk image file (.po, .dsk, .2mg, .hdv, " +
                        ".woz, .shk, .zip) or a directory (dynapro)"
        let work = sender.window
        let afterChoose: (URL) -> Void = { [weak self] u in
            let code = settings_ui_entry_disk_code(refs.entry)
            guard code >= 0 else { return }
            let ok = u.path.withCString { cptr in
                Int(settings_ui_disk_mount(Int32(code), cptr))
            }
            if ok == 0 {
                let alert = NSAlert()
                alert.messageText = "Could not mount"
                alert.informativeText =
                    "The emulator rejected the image at \(u.path)."
                alert.alertStyle = .warning
                alert.runModal()
            }
            refs.onChange()
            self?.notifyExternalChange()
        }
        if let w = work {
            panel.beginSheetModal(for: w) { resp in
                if resp == .OK, let u = panel.url { afterChoose(u) }
            }
        } else {
            if panel.runModal() == .OK, let u = panel.url { afterChoose(u) }
        }
    }

    @objc func onDiskEject(_ sender: NSButton) {
        guard let refs = diskRefs[ObjectIdentifier(sender)] else { return }
        let code = settings_ui_entry_disk_code(refs.entry)
        guard code >= 0 else { return }
        settings_ui_disk_eject(Int32(code))
        refs.onChange()
    }

    private func notifyExternalChange() {
        // Hook: future use for refreshing labels outside the window.
    }
}

// ---------------------------------------------------------------------------
// MARK: - C-string helpers
// ---------------------------------------------------------------------------

/// Call a C function that writes into a char buffer and returns the length
/// written (excluding NUL). Returns the resulting Swift String.
fileprivate func cString64(capacity: Int,
                            _ fill: (UnsafeMutablePointer<CChar>, Int32)
                                     -> Int32) -> String {
    let buf = UnsafeMutablePointer<CChar>.allocate(capacity: capacity)
    defer { buf.deallocate() }
    buf.initialize(repeating: 0, count: capacity)
    _ = fill(buf, Int32(capacity))
    return String(cString: buf)
}

/// Safely decodes an UnsafePointer<CChar> that may be NULL.
fileprivate func cString(_ p: UnsafePointer<CChar>?) -> String {
    guard let p = p else { return "" }
    return String(cString: p)
}
