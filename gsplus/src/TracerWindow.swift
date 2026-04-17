/**********************************************************************/
/*                    GSplus - Apple //gs Emulator                    */
/*                    Copyright 2025-2026 GSplus Contributors         */
/*                                                                    */
/*      This code is covered by the GNU GPL v3                        */
/*      See the file COPYING.txt or https://www.gnu.org/licenses/     */
/**********************************************************************/

import Cocoa

// "Debugger Tracer" — native F9 window that mirrors the GSBug/GoldenGate
// debugger layout. A polling timer snapshots register / memory state
// from the C side (via tracer_bridge.h) and redraws the panels.
//
// Panels, left column top-to-bottom:
//   - Registers (editable fields, N V M X D I Z C flag letters, M/Q bits)
//   - Direct Page hex dump (256 bytes at engine.direct)
//   - Stack hex dump (32 above / 64 below engine.stack)
//   - User memory view (address + length + hex/ASCII)
// Right column:
//   - Disassembly table (addr/bytes | cycles | M/X | mnemonic) with PC
//     row highlighted in yellow and auto-scroll to PC on halt.
// Top: toolbar of Pause / Run / Step Into / Step Over / Reset buttons.

// Pure Swift class — intentionally does NOT inherit from NSObject.
// NSObject subclasses with many Swift stored properties have unreliable
// ivar layout in Swift 4 per-file compilation mode, causing ARC crashes
// (same gotcha hit by DebugConsoleWindowController — see that file for
// the full story). Window lifecycle is handled via block-based
// NotificationCenter observers instead of NSWindowDelegate.
class TracerWindowController {

    private var window:   NSWindow?
    private var refreshTimer: Timer?
    private var embeddedConsole: DebugConsoleWindowController?

    // Toolbar
    private var btnPause:     NSButton!
    private var btnRun:       NSButton!
    private var btnStepInto:  NSButton!
    private var btnStepOver:  NSButton!
    private var btnReset:     NSButton!
    private var statusLabel:  NSTextField!

    // Registers
    private var regFields:    [String: NSTextField] = [:]
    private var flagLabels:   [String: NSTextField] = [:]   // N V M X D I Z C
    private var mStateLabel:  NSTextField!
    private var qStateLabel:  NSTextField!

    // Memory panels
    private var stackView:    StackFrameView!
    private var userAddrField: NSTextField!
    private var userLenField:  NSTextField!
    private var userView:      HexDumpView!

    // Disassembly
    private var disasmTable:  NSTableView!
    private var disasmRows:   [TracerRow] = []
    private var disasmAnchor: UInt32 = 0
    private var followPC:     Bool = true

    // Last snapshot
    private var lastRegs:     Tracer_regs = Tracer_regs()
    private var wasHalted:    Bool = false

    var isOpen: Bool { return window?.isVisible ?? false }

    init() {
        // Explicit empty init — forces the Swift 4 compiler to lay out
        // stored ivars consistently across files (same trick as
        // DebugConsoleWindowController).
        window          = nil
        refreshTimer    = nil
        regFields       = [:]
        flagLabels      = [:]
        disasmRows      = []
        disasmAnchor    = 0
        followPC        = true
        lastRegs        = Tracer_regs()
        wasHalted       = false
    }

    // MARK: - Public API

    func toggle() {
        if isOpen { close() } else { show() }
    }

    func show() {
        if window == nil { build() }
        refresh()
        startTimer()
        window?.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
    }

    func close() {
        stopTimer()
        window?.orderOut(nil)
    }

    // MARK: - Window construction

    // Roots for the grey-out-while-running state. Filled by build().
    private var leftContainer:  NSView!
    private var rightContainer: NSView!

    private func build() {
        let winW: CGFloat = 1220
        let winH: CGFloat = 920
        let toolbarH: CGFloat = 52
        let consoleH: CGFloat = 200
        let leftW: CGFloat = 360
        // Main working area height (between toolbar and console).
        let mainH: CGFloat = winH - toolbarH - consoleH

        let rect = NSRect(x: 120, y: 120, width: winW, height: winH)
        let style: NSWindow.StyleMask = [.titled, .closable,
                                          .resizable, .miniaturizable]
        let w = NSWindow(contentRect: rect, styleMask: style,
                         backing: .buffered, defer: false)
        w.title              = "GSplus Debugger Tracer"
        w.isReleasedWhenClosed = false

        NotificationCenter.default.addObserver(
            forName: NSWindow.willCloseNotification,
            object: w, queue: .main) { [weak self] _ in
            self?.stopTimer()
        }

        guard let content = w.contentView else { return }

        // Embedded debug console at the bottom (spans full width).
        let console = DebugConsoleWindowController()
        let consolePanel = NSView(frame: NSRect(
            x: 0, y: 0, width: winW, height: consoleH))
        consolePanel.autoresizingMask = [.width, .maxYMargin]
        content.addSubview(consolePanel)
        console.installInView(consolePanel)
        embeddedConsole = console

        // Left column container
        let leftRect = NSRect(x: 0, y: consoleH, width: leftW,
                              height: mainH)
        let leftView = NSView(frame: leftRect)
        leftView.autoresizingMask = [.height, .maxXMargin]
        content.addSubview(leftView)
        leftContainer = leftView

        // Right (disassembly) container
        let rightRect = NSRect(x: leftW, y: consoleH,
                               width: winW - leftW,
                               height: mainH)
        let rightView = NSView(frame: rightRect)
        rightView.autoresizingMask = [.width, .height]
        content.addSubview(rightView)
        rightContainer = rightView

        populateLeftColumn(in: leftView)
        populateDisassembly(in: rightView)

        // Toolbar at top, last so it's on top of z-order.
        let toolbar = makeToolbar(frame: NSRect(x: 0, y: winH - toolbarH,
                                                width: winW,
                                                height: toolbarH))
        toolbar.autoresizingMask = [.width, .minYMargin]
        content.addSubview(toolbar)

        window = w
    }

    private func makeToolbar(frame: NSRect) -> NSView {
        // Plain NSView with an explicit layer-backed fill and a visible
        // bottom separator. Explicit frames so buttons render reliably
        // even with the per-file Swift 4 compilation quirks.
        let bar = NSView(frame: frame)
        bar.wantsLayer = true
        bar.layer?.backgroundColor = NSColor(calibratedRed: 0.93,
                                              green: 0.93,
                                              blue:  0.94,
                                              alpha: 1.0).cgColor

        // Bottom separator
        let sep = NSBox(frame: NSRect(x: 0, y: 0,
                                      width: frame.width, height: 1))
        sep.boxType = .separator
        sep.autoresizingMask = [.width]
        bar.addSubview(sep)

        let specs: [(String, String, String, Selector, NSColor)] = [
            ("Pause",     "pause.fill",             "⏸",
             #selector(onPause(_:)),    .systemOrange),
            ("Run",       "play.fill",              "▶",
             #selector(onRun(_:)),      .systemGreen),
            ("Step Into", "arrow.turn.down.right",  "↳",
             #selector(onStepInto(_:)), .systemBlue),
            ("Step Over", "arrow.forward",          "→",
             #selector(onStepOver(_:)), .systemTeal),
            ("Reset",     "arrow.counterclockwise", "↺",
             #selector(onReset(_:)),    .systemRed),
        ]

        var x: CGFloat = 12
        let btnY: CGFloat = (frame.height - 30) / 2
        var made: [NSButton] = []
        for (title, symbol, fallback, sel, tint) in specs {
            let b = iconButton(title: title, symbol: symbol,
                               fallback: fallback, action: sel,
                               tint: tint)
            b.sizeToFit()
            var f = b.frame
            f.origin = NSPoint(x: x, y: btnY)
            // Enforce a minimum width so the icon+label always fits.
            f.size.width = max(f.width + 8, 108)
            f.size.height = 30
            b.frame = f
            bar.addSubview(b)
            made.append(b)
            x += f.width + 6
        }
        btnPause    = made[0]
        btnRun      = made[1]
        btnStepInto = made[2]
        btnStepOver = made[3]
        btnReset    = made[4]

        // Status label pinned to the right edge.
        let statusW: CGFloat = 320
        statusLabel = NSTextField(labelWithString: "Running")
        statusLabel.font = NSFont.systemFont(ofSize: 13, weight: .semibold)
        statusLabel.textColor = .secondaryLabelColor
        statusLabel.alignment = .right
        statusLabel.frame = NSRect(x: frame.width - statusW - 12,
                                   y: btnY + 5,
                                   width: statusW, height: 20)
        statusLabel.autoresizingMask = [.minXMargin]
        bar.addSubview(statusLabel)

        return bar
    }

    private func iconButton(title: String, symbol: String,
                            fallback: String, action: Selector,
                            tint: NSColor) -> NSButton {
        let b = NSButton(title: title, target: self, action: action)
        b.bezelStyle     = .rounded
        b.controlSize    = .regular
        b.imagePosition  = .imageLeading
        b.imageHugsTitle = true
        b.font           = NSFont.systemFont(ofSize: 12, weight: .semibold)
        if #available(macOS 11.0, *),
           let img = NSImage(systemSymbolName: symbol,
                             accessibilityDescription: title) {
            let cfg = NSImage.SymbolConfiguration(pointSize: 13,
                                                   weight: .semibold)
            b.image = img.withSymbolConfiguration(cfg)
            b.contentTintColor = tint
        } else {
            b.title = "\(fallback)  \(title)"
        }
        return b
    }

    private func populateLeftColumn(in container: NSView) {
        let regsH:  CGFloat = 220
        let stackH: CGFloat = 280
        let gap:    CGFloat = 14
        let cx:     CGFloat = 12
        let cw:     CGFloat = container.frame.width - 2 * cx
        let containerH = container.frame.height
        // Memory View card stretches from the bottom of the container up
        // to the bottom of the Stack card — so it shares its lower edge
        // with the disassembly panel on the right.
        let userBottomY: CGFloat = 12
        var topY = containerH - 12

        topY -= regsH
        let regsCard = buildRegistersCard(
            frame: NSRect(x: cx, y: topY, width: cw, height: regsH))
        regsCard.autoresizingMask = [.minYMargin]
        container.addSubview(regsCard)

        topY -= gap + stackH
        let stackCard = buildStackCard(
            frame: NSRect(x: cx, y: topY, width: cw, height: stackH))
        stackCard.autoresizingMask = [.minYMargin]
        container.addSubview(stackCard)

        let userTopY = topY - gap
        let userH = userTopY - userBottomY
        let userCard = buildUserMemCard(
            frame: NSRect(x: cx, y: userBottomY,
                          width: cw, height: userH))
        userCard.autoresizingMask = [.width, .height]
        container.addSubview(userCard)
    }

    /// Plain bordered container with a title label at the top and a
    /// content view pre-sized below it. Replaces NSBox so sizing is
    /// predictable across macOS versions.
    private func makeCard(title: String, frame: NSRect) -> (NSView, NSView) {
        let card = CardView(frame: frame)
        card.wantsLayer = true
        card.layer?.borderColor  = NSColor.separatorColor.cgColor
        card.layer?.borderWidth  = 1
        card.layer?.cornerRadius = 6
        card.layer?.backgroundColor = NSColor.controlBackgroundColor.cgColor

        let titleH: CGFloat = 20
        let titleLbl = NSTextField(labelWithString: title)
        titleLbl.font = NSFont.boldSystemFont(ofSize: 12)
        titleLbl.textColor = .secondaryLabelColor
        titleLbl.frame = NSRect(x: 10, y: frame.height - titleH - 2,
                                width: frame.width - 20, height: titleH)
        titleLbl.autoresizingMask = [.minYMargin, .width]
        card.addSubview(titleLbl)

        let contentH = frame.height - titleH - 4
        let content  = NSView(frame: NSRect(x: 6, y: 4,
                                            width: frame.width - 12,
                                            height: contentH))
        content.autoresizingMask = [.width, .height]
        card.addSubview(content)
        return (card, content)
    }

    private func buildStackCard(frame: NSRect) -> NSView {
        let (card, content) = makeCard(title: "Stack (SP = middle row)",
                                        frame: frame)
        let view = StackFrameView(frame: content.bounds)
        view.autoresizingMask = [.width, .height]
        content.addSubview(view)
        stackView = view
        return card
    }

    // MARK: Registers card

    private func buildRegistersCard(frame: NSRect) -> NSView {
        let (card, content) = makeCard(title: "Registers", frame: frame)
        let font = NSFont.monospacedSystemFont(ofSize: 12, weight: .regular)

        let leftRows: [(String, Int)] = [
            ("P",  2), ("A",  4), ("X",  4),
            ("Y",  4), ("DP", 4), ("M",  2),
        ]
        let rightRows: [(String, Int)] = [
            ("PC", 6), ("K",  2), ("B",  2),
            ("SP", 4), ("Q",  2),
        ]

        let rowH: CGFloat = 22
        let flagsRowH: CGFloat = 22
        let contentH = content.bounds.height

        // Pack rows from the top of the content area downward. Last row
        // sits above the flag-letter strip at the bottom.
        let firstRowTopY = contentH - 4  // top of first row
        let firstRowBottomY = firstRowTopY - rowH

        func addField(label: String, hexDigits: Int, x: CGFloat, y: CGFloat,
                      wLabel: CGFloat, wField: CGFloat,
                      editable: Bool) -> NSTextField {
            let lbl = NSTextField(labelWithString: "\(label):")
            lbl.font = font
            lbl.alignment = .right
            lbl.textColor = .secondaryLabelColor
            lbl.frame = NSRect(x: x, y: y + 2, width: wLabel, height: rowH - 4)
            content.addSubview(lbl)

            let tf = NSTextField(frame:
                NSRect(x: x + wLabel + 4, y: y + 1,
                       width: wField, height: rowH - 2))
            tf.font = font
            tf.alignment = .left
            tf.isEditable       = editable
            tf.isSelectable     = true
            tf.isBezeled        = editable
            tf.drawsBackground  = editable
            tf.backgroundColor  = .textBackgroundColor
            tf.textColor        = .labelColor
            tf.stringValue      = String(repeating: "0", count: hexDigits)
            tf.target           = self
            tf.action           = #selector(onRegEdited(_:))
            tf.identifier       = NSUserInterfaceItemIdentifier(label)
            content.addSubview(tf)
            return tf
        }

        // Left column — 6 rows starting at top
        var y = firstRowBottomY
        for (lbl, hex) in leftRows {
            let editable = lbl != "M"
            regFields[lbl] = addField(label: lbl, hexDigits: hex,
                                       x: 6, y: y,
                                       wLabel: 28, wField: 70,
                                       editable: editable)
            y -= rowH
        }

        // Right column — 5 rows, starting at top aligned with left
        y = firstRowBottomY
        let rxLabel: CGFloat = 170
        for (lbl, hex) in rightRows {
            let editable = lbl != "Q"
            let wField: CGFloat = (hex == 6) ? 70 : (hex == 4 ? 58 : 34)
            regFields[lbl] = addField(label: lbl, hexDigits: hex,
                                       x: rxLabel, y: y,
                                       wLabel: 28, wField: wField,
                                       editable: editable)
            y -= rowH
        }

        // Flag-letter strip: full-width row at the bottom of the card.
        let flagY: CGFloat = 2
        let names = ["N","V","M","X","D","I","Z","C"]
        // Spread evenly across the content width.
        let flagStride = (content.bounds.width - 20) /
                         CGFloat(names.count)
        for (i, n) in names.enumerated() {
            let lb = NSTextField(labelWithString: n)
            lb.font = NSFont.monospacedSystemFont(ofSize: 13,
                                                   weight: .bold)
            lb.alignment = .center
            lb.frame = NSRect(
                x: 10 + CGFloat(i) * flagStride,
                y: flagY,
                width: flagStride, height: flagsRowH)
            content.addSubview(lb)
            flagLabels[n] = lb
        }

        mStateLabel = regFields["M"]
        qStateLabel = regFields["Q"]

        return card
    }

    // MARK: Hex dump card (DP/Stack)

    private func buildHexCard(title: String, frame: NSRect,
                              hexSetter: (HexDumpView) -> Void) -> NSView {
        let card = titledBox(title: title, frame: frame)
        let content = card.contentView!

        let dump = HexDumpView(frame: content.bounds)
        dump.autoresizingMask = [.width, .height]
        content.addSubview(dump)
        hexSetter(dump)
        return card
    }

    // MARK: User memory card

    private func buildUserMemCard(frame: NSRect) -> NSView {
        let (card, content) = makeCard(title: "Memory View", frame: frame)

        let rowTop = content.bounds.height - 22
        let addrLbl = NSTextField(labelWithString: "Address:")
        addrLbl.frame = NSRect(x: 6, y: rowTop + 2,
                               width: 64, height: 18)
        addrLbl.textColor = .secondaryLabelColor
        content.addSubview(addrLbl)

        userAddrField = NSTextField(frame:
            NSRect(x: 74, y: content.bounds.height - 28, width: 90, height: 22))
        userAddrField.font = NSFont.monospacedSystemFont(ofSize: 12,
                                                         weight: .regular)
        userAddrField.stringValue = "$E10000"
        userAddrField.target = self
        userAddrField.action = #selector(onUserAddrChanged(_:))
        content.addSubview(userAddrField)

        let lenLbl = NSTextField(labelWithString: "Bytes:")
        lenLbl.frame = NSRect(x: 172, y: content.bounds.height - 26,
                              width: 44, height: 18)
        lenLbl.textColor = .secondaryLabelColor
        content.addSubview(lenLbl)

        userLenField = NSTextField(frame:
            NSRect(x: 216, y: content.bounds.height - 28, width: 54, height: 22))
        userLenField.font = NSFont.monospacedSystemFont(ofSize: 12,
                                                        weight: .regular)
        // Empty default = auto-fill to the dump's visible height.
        userLenField.stringValue = ""
        userLenField.placeholderString = "auto"
        userLenField.target = self
        userLenField.action = #selector(onUserAddrChanged(_:))
        content.addSubview(userLenField)

        // Dump fills everything below the address row with a 2 px gap.
        let dumpH = rowTop - 2
        let dumpFrame = NSRect(x: 0, y: 0,
                               width: content.bounds.width,
                               height: dumpH)
        userView = HexDumpView(frame: dumpFrame)
        userView.autoresizingMask = [.width, .height]
        content.addSubview(userView)
        return card
    }

    // MARK: Disassembly

    private func populateDisassembly(in container: NSView) {
        let (card, content) = makeCard(title: "Disassembly (PC centred)",
            frame: NSRect(x: 4, y: 12,
                          width: container.frame.width - 16,
                          height: container.frame.height - 24))
        card.autoresizingMask = [.width, .height]
        container.addSubview(card)

        let scroll = NSScrollView(frame: content.bounds)
        scroll.autoresizingMask        = [.width, .height]
        scroll.hasVerticalScroller     = true
        scroll.hasHorizontalScroller   = false
        scroll.autohidesScrollers      = false
        scroll.borderType              = .noBorder

        let tv = NSTableView(frame: content.bounds)
        tv.autoresizingMask   = [.width, .height]
        tv.usesAlternatingRowBackgroundColors = false
        tv.rowSizeStyle       = .custom
        tv.rowHeight          = 18
        tv.gridStyleMask      = []
        tv.headerView         = NSTableHeaderView()
        tv.allowsMultipleSelection = false
        tv.backgroundColor    = NSColor.controlBackgroundColor

        let colSpecs: [(String, CGFloat)] = [
            ("Address / Bytes", 210),
            ("Cyc", 42),
            ("M/X", 52),
            ("Disassembly", 260),
            ("Symbol", 320),
        ]
        for (title, w) in colSpecs {
            let col = NSTableColumn(
                identifier: NSUserInterfaceItemIdentifier(title))
            col.title = title
            col.width = w
            col.minWidth = 36
            tv.addTableColumn(col)
        }

        tv.dataSource = TracerTableBridge.shared
        tv.delegate   = TracerTableBridge.shared
        TracerTableBridge.shared.owner = self

        scroll.documentView = tv
        content.addSubview(scroll)
        disasmTable = tv
    }

    // MARK: - Timer / refresh
    //
    // We don't poll the full emulator state on a fast timer any more.
    // Instead, every 500 ms we just check whether the halt flag has
    // transitioned (breakpoint hit, step completed, external halt). If
    // it has, we do one full refresh; otherwise we do nothing. The
    // panels are also repainted immediately whenever the user clicks a
    // toolbar button.

    private func startTimer() {
        stopTimer()
        refreshTimer = Timer.scheduledTimer(withTimeInterval: 0.5,
                                             repeats: true) { [weak self] _ in
            self?.tickHaltWatcher()
        }
    }

    private func stopTimer() {
        refreshTimer?.invalidate()
        refreshTimer = nil
    }

    private func tickHaltWatcher() {
        let halted = tracer_is_halted() != 0
        if halted != wasHalted {
            refresh()
        }
        // Also feed the embedded debug console so new log lines show up
        // even when we don't otherwise refresh.
        embeddedConsole?.updateIfNeeded()
    }

    // MARK: - Refresh

    private func refresh() {
        var regs = Tracer_regs()
        tracer_get_registers(&regs)
        lastRegs = regs

        let halted = regs.halted != 0
        // Pause only makes sense while running; Run only while halted.
        // Step buttons need the CPU parked. Reset is always fine.
        btnPause.isEnabled    = !halted
        btnRun.isEnabled      =  halted
        btnReset.isEnabled    = true
        btnStepInto.isEnabled = halted
        btnStepOver.isEnabled = halted

        statusLabel.stringValue = halted
            ? (regs.stepping != 0 ? "Halted (stepping)" : "Halted")
            : "Running — click Pause to inspect state"
        statusLabel.textColor = halted ? .systemOrange : .systemGreen

        // Always refresh panels so they display a snapshot regardless of
        // run state. The status label above indicates whether values are
        // live (halted) or the last captured state (running).
        refreshRegisters(regs: regs)
        refreshStack(regs: regs)

        // Memory View: auto-follow the current instruction's target
        // address when we can decode it; otherwise use the user-typed
        // address.
        var target = Tracer_target()
        let hasTarget = tracer_decode_target(regs.pc,
                                              regs.flag_m8 != 0 ? 1 : 0,
                                              regs.flag_x8 != 0 ? 1 : 0,
                                              &target) != 0
        // Auto-size byte count to fill the dump view's visible height.
        // User-typed overrides auto, but is clamped sensibly.
        let rowH: CGFloat = 14
        let rowsAvailable = max(4,
            Int((userView?.bounds.height ?? 200) / rowH))
        let autoBytes = rowsAvailable * 16
        let userTyped = parseInt(userLenField.stringValue) ?? 0
        let len = userTyped > 0
            ? min(max(userTyped, 16), 4096)
            : autoBytes
        if hasTarget {
            refreshHexPanelWithTarget(addr: target.effective_addr,
                                       len: len,
                                       target: target)
            userAddrField.stringValue = String(format: "$%06X",
                                               target.effective_addr)
        } else {
            if let addr = parseAddress(userAddrField.stringValue) {
                refreshHexPanel(userView, addr: addr, len: len)
            }
            userView?.setHighlights([])
        }

        disasmAnchor = regs.pc
        rebuildDisasm(anchor: regs.pc,
                      m8: regs.flag_m8 != 0,
                      x8: regs.flag_x8 != 0)
        disasmTable.reloadData()
        scrollDisasmToPC()

        wasHalted = halted
    }

    /// Read 20 rows' worth of stack around SP and hand them to the view.
    /// Native mode (e=0) shows 2-byte cells, emulation mode (e=1) shows
    /// single bytes. SP lands in the middle row; rows above are SP-cell,
    /// SP-2*cell, ...; rows below are SP+cell, SP+2*cell, ...
    private func refreshStack(regs: Tracer_regs) {
        let nativeMode = regs.flag_e == 0
        let cell: UInt32 = nativeMode ? 2 : 1
        let rowsBelow = 10   // rows ABOVE in address terms (higher SP values)
        let rowsAbove = 9    // rows BELOW (pushed data — lower addresses …)
        let sp = UInt32(regs.sp)
        // First address we need = SP - rowsAbove*cell (lowest), last = SP + rowsBelow*cell
        // 20 rows total = 1 (SP) + rowsAbove + rowsBelow.
        let lowAddr = sp &- UInt32(rowsAbove) &* cell
        let totalBytes = Int((UInt32(rowsAbove) + 1 + UInt32(rowsBelow))
                             * cell)
        var buf = [UInt8](repeating: 0, count: totalBytes)
        buf.withUnsafeMutableBufferPointer { b in
            tracer_read_memory(lowAddr, b.baseAddress, Int32(totalBytes))
        }
        stackView?.update(sp: sp, nativeMode: nativeMode,
                          lowAddr: lowAddr, bytes: buf,
                          rowsAbove: rowsAbove, rowsBelow: rowsBelow)
    }

    private func refreshRegisters(regs: Tracer_regs) {
        func setField(_ k: String, _ fmt: String, _ v: UInt) {
            if let f = regFields[k] {
                // Don't overwrite while the user is editing
                if f.currentEditor() == nil {
                    f.stringValue = String(format: fmt, v)
                }
            }
        }
        setField("PC", "%06X", UInt(regs.pc))
        setField("A",  "%04X", UInt(regs.a))
        setField("X",  "%04X", UInt(regs.x))
        setField("Y",  "%04X", UInt(regs.y))
        setField("SP", "%04X", UInt(regs.sp))
        setField("DP", "%04X", UInt(regs.dp))
        setField("P",  "%02X", UInt(regs.psr & 0xFF))
        setField("K",  "%02X", UInt(regs.pbank))
        setField("B",  "%02X", UInt(regs.dbank))

        // M/Q as 8-bit binary
        if let f = regFields["M"] { f.stringValue = binary8(regs.m_state) }
        if let f = regFields["Q"] { f.stringValue = binary8(regs.q_state) }

        // Flag letters: bold when set, grey when unset
        let psr = Int(regs.psr)
        let bits: [(String, Int)] = [
            ("N", 0x80), ("V", 0x40),
            ("M", 0x20), ("X", 0x10),
            ("D", 0x08), ("I", 0x04),
            ("Z", 0x02), ("C", 0x01),
        ]
        for (name, mask) in bits {
            if let lb = flagLabels[name] {
                let set = (psr & mask) != 0
                lb.textColor = set ? .labelColor
                                   : NSColor(calibratedWhite: 0.7, alpha: 1.0)
                lb.font = NSFont.monospacedSystemFont(ofSize: 12,
                    weight: set ? .bold : .regular)
            }
        }
    }

    private func refreshHexPanel(_ v: HexDumpView?, addr: UInt32, len: Int) {
        guard let v = v else { return }
        var buf = [UInt8](repeating: 0, count: len)
        buf.withUnsafeMutableBufferPointer { b in
            tracer_read_memory(addr, b.baseAddress, Int32(len))
        }
        v.setData(address: addr, bytes: buf)
        v.setHighlights([])
    }

    /// Reads memory around the instruction's target, centers it in the
    /// view, and paints two highlight bands:
    ///   - base_addr in teal (the operand's raw address)
    ///   - effective_addr in orange (base + index register)
    /// If the instruction is non-indexed, only the effective band is
    /// drawn (since base == effective).
    private func refreshHexPanelWithTarget(addr: UInt32, len: Int,
                                            target: Tracer_target) {
        guard let v = userView else { return }
        // Align the start address on a 16-byte boundary that sits a few
        // rows before the effective address, so the target is visible
        // but not at the very first row.
        let pre: UInt32 = 48
        var start: UInt32 = 0
        if addr >= pre { start = (addr - pre) & 0xFFFFF0 }
        else           { start = 0 }

        var buf = [UInt8](repeating: 0, count: len)
        buf.withUnsafeMutableBufferPointer { b in
            tracer_read_memory(start, b.baseAddress, Int32(len))
        }
        v.setData(address: start, bytes: buf)

        var highlights: [HexHighlight] = []
        let width = Int(target.width == 0 ? 1 : target.width)

        if target.is_indexed != 0 {
            // Base band first (may be outside the window — guard below)
            let baseOff = Int(Int64(target.base_addr) - Int64(start))
            if baseOff >= 0 && baseOff + width <= buf.count {
                highlights.append(HexHighlight(
                    byteOffset: baseOff, byteCount: width,
                    color: NSColor.systemTeal.withAlphaComponent(0.45)))
            }
        }
        let effOff = Int(Int64(target.effective_addr) - Int64(start))
        if effOff >= 0 && effOff + width <= buf.count {
            highlights.append(HexHighlight(
                byteOffset: effOff, byteCount: width,
                color: NSColor.systemOrange.withAlphaComponent(0.55)))
        }
        v.setHighlights(highlights)
    }

    private func rebuildDisasm(anchor: UInt32, m8: Bool, x8: Bool) {
        let count = 80
        var raw = [Tracer_dasm](repeating: Tracer_dasm(), count: count)
        let n = Int(raw.withUnsafeMutableBufferPointer { b in
            tracer_disasm(anchor, m8 ? 1 : 0, x8 ? 1 : 0,
                          b.baseAddress, Int32(count))
        })
        disasmRows = (0..<n).map { TracerRow.from(raw[$0]) }
    }

    private func scrollDisasmToPC() {
        let pc = lastRegs.pc
        guard let idx = disasmRows.firstIndex(where: { $0.pc == pc })
            else { return }
        disasmTable.scrollRowToVisible(max(0, idx - 4))
    }

    // MARK: - Actions

    @objc private func onPause(_ sender: Any?) {
        tracer_pause()
        refresh()
    }
    @objc private func onRun(_ sender: Any?) {
        tracer_run()
        refresh()  // immediate grey-out
    }
    @objc private func onStepInto(_ sender: Any?) {
        tracer_step_into()
        // Emulator runs one instruction asynchronously then halts again.
        // Give the run loop a tick to complete before snapshotting.
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.05) { [weak self] in
            self?.refresh()
        }
    }
    @objc private func onStepOver(_ sender: Any?) {
        tracer_step_over()
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.05) { [weak self] in
            self?.refresh()
        }
    }
    @objc private func onReset(_ sender: Any?) {
        tracer_reset()
        refresh()
    }

    @objc private func onRegEdited(_ sender: NSTextField) {
        guard let id = sender.identifier?.rawValue else { return }
        let raw = sender.stringValue.trimmingCharacters(in: .whitespaces)
        guard let v = parseInt(raw) else {
            NSSound.beep()
            refresh()
            return
        }
        switch id {
        case "PC": tracer_set_pc(UInt32(v & 0xffffff))
        case "A":  tracer_set_a(UInt16(v & 0xffff))
        case "X":  tracer_set_x(UInt16(v & 0xffff))
        case "Y":  tracer_set_y(UInt16(v & 0xffff))
        case "SP": tracer_set_sp(UInt16(v & 0xffff))
        case "DP": tracer_set_dp(UInt16(v & 0xffff))
        case "P":  tracer_set_psr(UInt16(v & 0x1ff))
        case "K":  tracer_set_pbank(UInt8(v & 0xff))
        case "B":  tracer_set_dbank(UInt8(v & 0xff))
        default: break
        }
        refresh()
    }

    @objc fileprivate func onUserAddrChanged(_ sender: Any?) {
        refresh()
    }

    // MARK: - Parsing helpers

    fileprivate func parseAddress(_ s: String) -> UInt32? {
        if let v = parseInt(s) { return UInt32(v & 0xffffff) }
        return nil
    }

    fileprivate func parseInt(_ raw: String) -> Int? {
        let s = raw.trimmingCharacters(in: .whitespaces)
        if s.isEmpty { return nil }
        if s.hasPrefix("$") { return Int(s.dropFirst(), radix: 16) }
        if s.hasPrefix("0x") || s.hasPrefix("0X") {
            return Int(s.dropFirst(2), radix: 16)
        }
        return Int(s)
    }

    fileprivate func binary8(_ v: UInt8) -> String {
        var out = ""
        for i in (0..<8).reversed() {
            out += ((Int(v) >> i) & 1) == 1 ? "1" : "0"
        }
        return out
    }

    // MARK: - Helpers

    private func titledBox(title: String, frame: NSRect) -> NSBox {
        let box = NSBox(frame: frame)
        box.title = title
        box.titlePosition = .atTop
        box.boxType = .primary
        return box
    }

    // Expose rows + lastRegs to the table bridge
    fileprivate var _rows: [TracerRow]       { return disasmRows }
    fileprivate var _pc:   UInt32            { return lastRegs.pc }
}

// ---------------------------------------------------------------------------
// MARK: - TracerRow (Swift-side copy of a disassembled instruction)
// ---------------------------------------------------------------------------

fileprivate struct TracerRow {
    let pc:       UInt32
    let bytes:    String    // "XX XX XX"
    let cycles:   UInt8
    let mxText:   String    // "Mm Xx"
    let mnemonic: String

    static func from(_ d: Tracer_dasm) -> TracerRow {
        var bytesStr = ""
        let n = Int(d.nbytes)
        let arr = [d.bytes.0, d.bytes.1, d.bytes.2, d.bytes.3]
        for i in 0..<4 {
            if i < n { bytesStr += String(format: "%02X ", arr[i]) }
            else     { bytesStr += "   " }
        }
        let mx = String(format: "%@ %@",
                        d.m8 != 0 ? "m" : "M",
                        d.x8 != 0 ? "x" : "X")
        let mnem = withUnsafePointer(to: d.mnemonic) { tup in
            tup.withMemoryRebound(to: CChar.self, capacity: 48) {
                String(cString: $0)
            }
        }
        return TracerRow(pc: d.pc,
                         bytes: bytesStr.trimmingCharacters(in: .whitespaces),
                         cycles: d.cycles,
                         mxText: mx,
                         mnemonic: mnem)
    }
}

// ---------------------------------------------------------------------------
// MARK: - Disassembly table data source
// ---------------------------------------------------------------------------

fileprivate class TracerTableBridge: NSObject,
        NSTableViewDataSource, NSTableViewDelegate {
    static let shared = TracerTableBridge()
    weak var owner: TracerWindowController?

    func numberOfRows(in tableView: NSTableView) -> Int {
        return owner?._rows.count ?? 0
    }

    func tableView(_ tv: NSTableView,
                   viewFor col: NSTableColumn?, row: Int) -> NSView? {
        guard let rows = owner?._rows,
              row >= 0 && row < rows.count,
              let col = col else { return nil }
        let r = rows[row]
        let cell = NSTableCellView()
        let tf = NSTextField(labelWithString: "")
        tf.font = NSFont.monospacedSystemFont(ofSize: 12, weight: .regular)
        tf.translatesAutoresizingMaskIntoConstraints = false
        cell.addSubview(tf)
        NSLayoutConstraint.activate([
            tf.leadingAnchor.constraint(equalTo: cell.leadingAnchor,
                                        constant: 4),
            tf.trailingAnchor.constraint(equalTo: cell.trailingAnchor,
                                         constant: -4),
            tf.centerYAnchor.constraint(equalTo: cell.centerYAnchor),
        ])
        cell.textField = tf

        switch col.identifier.rawValue {
        case "Address / Bytes":
            tf.stringValue =
                String(format: "%02X/%04X: %@",
                       (r.pc >> 16) & 0xFF, r.pc & 0xFFFF, r.bytes)
        case "Cyc":
            tf.stringValue = String(r.cycles)
        case "M/X":
            tf.stringValue = r.mxText
        case "Symbol":
            if let cstr = symbols_describe_pc(r.pc) {
                tf.stringValue = String(cString: cstr)
                tf.textColor = .systemPurple
            } else {
                tf.stringValue = ""
                tf.textColor = .secondaryLabelColor
            }
        case "Disassembly":
            tf.stringValue = r.mnemonic
        default:
            tf.stringValue = ""
        }
        return cell
    }

    func tableView(_ tv: NSTableView, rowViewForRow row: Int) -> NSTableRowView? {
        guard let rows = owner?._rows,
              row >= 0 && row < rows.count else { return nil }
        let v = TracerRowView()
        v.isPC = rows[row].pc == owner?._pc
        return v
    }
}

fileprivate class TracerRowView: NSTableRowView {
    var isPC: Bool = false
    override func drawBackground(in dirtyRect: NSRect) {
        if isPC {
            NSColor.systemYellow.withAlphaComponent(0.35).setFill()
            dirtyRect.fill()
        } else {
            super.drawBackground(in: dirtyRect)
        }
    }
}

// ---------------------------------------------------------------------------
// MARK: - CardView
// ---------------------------------------------------------------------------

/// Plain rounded-rectangle container with layer-backed border and fill.
/// Used in place of NSBox for predictable sizing/appearance on recent
/// macOS versions.
class CardView: NSView {
    override var wantsUpdateLayer: Bool { return true }
}

// ---------------------------------------------------------------------------
// MARK: - HexDumpView
// ---------------------------------------------------------------------------

struct HexHighlight {
    let byteOffset: Int
    let byteCount:  Int
    let color:      NSColor
}

/// Read-only monospace hex + ASCII dump with optional per-byte background
/// highlights (used to visualise memory-access targets).
class HexDumpView: NSView {
    private var baseAddress: UInt32 = 0
    private var data: [UInt8] = []
    private var highlights: [HexHighlight] = []
    private var bytesPerRow: Int = 16

    override var isFlipped: Bool { return true }

    func setData(address: UInt32, bytes: [UInt8]) {
        self.baseAddress = address
        self.data = bytes
        needsDisplay = true
    }

    func setHighlights(_ hs: [HexHighlight]) {
        self.highlights = hs
        needsDisplay = true
    }

    override func draw(_ dirtyRect: NSRect) {
        let bg = NSColor.textBackgroundColor
        bg.setFill()
        dirtyRect.fill()

        let font     = NSFont.monospacedSystemFont(ofSize: 11, weight: .regular)
        let attrs: [NSAttributedString.Key: Any] = [
            .font: font, .foregroundColor: NSColor.labelColor,
        ]
        let mutedAttrs: [NSAttributedString.Key: Any] = [
            .font: font, .foregroundColor: NSColor.secondaryLabelColor,
        ]

        // Measure one monospace character so we can compute byte-column
        // positions precisely for highlight rects.
        let charW = NSAttributedString(string: "0", attributes: attrs)
            .size().width
        let hexColX: CGFloat = 64
        let rowH: CGFloat = 14

        // Paint highlights first, behind the text.
        for h in highlights {
            for bi in 0 ..< h.byteCount {
                let off = h.byteOffset + bi
                if off < 0 || off >= data.count { continue }
                let row  = off / bytesPerRow
                let col  = off % bytesPerRow
                let x = hexColX + CGFloat(col) * (3 * charW)
                let y = 2 + CGFloat(row) * rowH
                let rect = NSRect(x: x - 1, y: y - 1,
                                  width: 2 * charW + 2,
                                  height: rowH - 0)
                h.color.setFill()
                rect.fill()
            }
        }

        // Then draw the text on top.
        var y: CGFloat = 2
        var i = 0
        while i < data.count {
            let end = min(i + bytesPerRow, data.count)
            let addr = baseAddress &+ UInt32(i)
            let addrStr = String(format: "%06X: ", addr)

            var hex = ""
            var ascii = ""
            for j in i..<end {
                hex += String(format: "%02X ", data[j])
                let b = data[j]
                ascii += (b >= 0x20 && b < 0x7F)
                    ? String(UnicodeScalar(b)) : "."
            }
            let pad = (bytesPerRow - (end - i)) * 3
            if pad > 0 { hex += String(repeating: " ", count: pad) }

            addrStr.draw(at: NSPoint(x: 4, y: y), withAttributes: mutedAttrs)
            hex.draw(at: NSPoint(x: hexColX, y: y), withAttributes: attrs)
            ascii.draw(
                at: NSPoint(
                    x: hexColX + CGFloat(bytesPerRow) * 3 * charW + 6,
                    y: y),
                withAttributes: mutedAttrs)

            i += bytesPerRow
            y += rowH
        }
    }
}

// ---------------------------------------------------------------------------
// MARK: - StackFrameView
// ---------------------------------------------------------------------------

/// Stack panel: 20 rows with addresses and either a single byte
/// (emulation mode) or a little-endian word (native mode). SP sits on
/// the middle row and is highlighted.
class StackFrameView: NSView {
    private var sp: UInt32 = 0
    private var nativeMode: Bool = false
    private var lowAddr: UInt32 = 0
    private var bytes: [UInt8] = []
    private var rowsAbove: Int = 9
    private var rowsBelow: Int = 10

    override var isFlipped: Bool { return true }

    func update(sp: UInt32, nativeMode: Bool, lowAddr: UInt32,
                bytes: [UInt8], rowsAbove: Int, rowsBelow: Int) {
        self.sp         = sp
        self.nativeMode = nativeMode
        self.lowAddr    = lowAddr
        self.bytes      = bytes
        self.rowsAbove  = rowsAbove
        self.rowsBelow  = rowsBelow
        needsDisplay    = true
    }

    override func draw(_ dirtyRect: NSRect) {
        NSColor.textBackgroundColor.setFill()
        dirtyRect.fill()

        let cell: Int = nativeMode ? 2 : 1
        let totalRows = rowsAbove + 1 + rowsBelow
        let rowH: CGFloat = max(14, (bounds.height - 4) / CGFloat(totalRows))
        let attrs: [NSAttributedString.Key: Any] = [
            .font: NSFont.monospacedSystemFont(ofSize: 11, weight: .regular),
            .foregroundColor: NSColor.labelColor,
        ]
        let addrAttrs: [NSAttributedString.Key: Any] = [
            .font: NSFont.monospacedSystemFont(ofSize: 11, weight: .regular),
            .foregroundColor: NSColor.secondaryLabelColor,
        ]

        // Rows laid out top-to-bottom with the *highest* address at the
        // top (matches the GoldenGate/GSBug stack view convention) and
        // SP in the middle. rowIndex 0 = highest addr, totalRows-1 = lowest.
        // All arithmetic uses signed Ints and wrapping UInt32 so a low
        // SP doesn't underflow when we display rows below it.
        for rowIndex in 0 ..< totalRows {
            let dCells    = rowsBelow - rowIndex
            let addr       = sp &+ UInt32(bitPattern: Int32(dCells * cell))
            let addrMasked = addr & 0xFFFF
            let byteOffset = (rowsAbove + dCells) * cell

            let y: CGFloat = 2 + CGFloat(rowIndex) * rowH
            let rowRect = NSRect(x: 0, y: y - 1,
                                 width: bounds.width, height: rowH)

            if rowIndex == rowsBelow {
                NSColor.systemYellow.withAlphaComponent(0.35).setFill()
                rowRect.fill()
            }

            let addrStr = String(format: "%04X", addr & 0xFFFF)
            addrStr.draw(at: NSPoint(x: 10, y: y + 1),
                         withAttributes: addrAttrs)

            var valStr = "--"
            if byteOffset >= 0 && byteOffset + cell <= bytes.count {
                if cell == 2 {
                    let lo = UInt16(bytes[byteOffset])
                    let hi = UInt16(bytes[byteOffset + 1])
                    valStr = String(format: "%04X", (hi << 8) | lo)
                } else {
                    valStr = String(format: "%02X",
                                    bytes[byteOffset])
                }
            }
            valStr.draw(at: NSPoint(x: 74, y: y + 1),
                        withAttributes: attrs)

            if rowIndex == rowsBelow {
                "← SP".draw(at: NSPoint(x: 160, y: y + 1),
                            withAttributes: addrAttrs)
            }
        }
    }
}
