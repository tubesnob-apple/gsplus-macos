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
//   - Stack hex dump (32 above / 64 below engine.stack) — fills remaining space
// Right column:
//   - Disassembly table (addr/bytes | cycles | M/X | mnemonic) with PC
//     row highlighted in yellow and auto-scroll to PC on halt.
// Top: toolbar of Pause / Run / Step One / Reset (left) and View Memory
// (right). Emulator state is reflected in the window title, e.g.
// "GSplus Debugger Tracer [Halted]".

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
    private var btnReset:     NSButton!
    private var btnViewMem:   NSButton!

    // Registers
    private var regFields:    [String: NSTextField] = [:]
    private var flagLabels:   [String: NSTextField] = [:]   // N V M X D I Z C
    private var mStateLabel:  NSTextField!
    private var qStateLabel:  NSTextField!

    // Memory panels
    private var stackView:    StackFrameView!

    // Detached memory viewer (opened from the toolbar)
    private var memViewer:    MemoryViewerWindowController?

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

    /// Called from the main run loop every emulator tick. Auto-opens
    /// the Tracer window the first time the emulator halts (breakpoint,
    /// WDM trap, STP, user pause) — this replaces the legacy F8
    /// console's auto-open-on-halt behavior. Opens once per halt; when
    /// the CPU runs again, the window stays put until the user closes
    /// it explicitly.
    func updateIfNeeded() {
        if !isOpen && tracer_is_halted() != 0 {
            show()
        }
    }

    // MARK: - Window construction

    // Roots for the grey-out-while-running state. Filled by build().
    private var leftContainer:  NSView!
    private var rightContainer: NSView!

    private func build() {
        let winW: CGFloat = 1500
        let winH: CGFloat = 950
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
            ("Step One",  "arrow.turn.down.right",  "↳",
             #selector(onStepInto(_:)), .systemBlue),
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
        btnReset    = made[3]

        // "View Memory" pinned to the right edge. Always enabled — the
        // viewer window can open at any time; it just shows stale data
        // (and says so) while the emulator is running.
        let viewMemBtn = iconButton(title: "View Memory",
                                     symbol: "magnifyingglass",
                                     fallback: "🔍",
                                     action: #selector(onViewMemory(_:)),
                                     tint: .systemPurple)
        viewMemBtn.sizeToFit()
        var vf = viewMemBtn.frame
        vf.size.width  = max(vf.width + 8, 130)
        vf.size.height = 30
        vf.origin = NSPoint(x: frame.width - vf.width - 12, y: btnY)
        viewMemBtn.frame = vf
        viewMemBtn.autoresizingMask = [.minXMargin]
        bar.addSubview(viewMemBtn)
        btnViewMem = viewMemBtn

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
        let gap:    CGFloat = 14
        let cx:     CGFloat = 12
        let cw:     CGFloat = container.frame.width - 2 * cx
        let containerH = container.frame.height
        let stackBottomY: CGFloat = 12
        var topY = containerH - 12

        topY -= regsH
        let regsCard = buildRegistersCard(
            frame: NSRect(x: cx, y: topY, width: cw, height: regsH))
        regsCard.autoresizingMask = [.minYMargin]
        container.addSubview(regsCard)

        // Stack card now fills the rest of the left column down to the
        // bottom gutter — the old user-memory panel lived below it.
        let stackTopY = topY - gap
        let stackH = stackTopY - stackBottomY
        let stackCard = buildStackCard(
            frame: NSRect(x: cx, y: stackBottomY,
                          width: cw, height: stackH))
        stackCard.autoresizingMask = [.width, .height]
        container.addSubview(stackCard)
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

        // Reserve a strip at the top for the title label with no overlap
        // into the content area. The Stack and Memory subviews fill their
        // content bounds opaquely, so an overlapping content origin lets
        // them overpaint the title text.
        let titleH: CGFloat = 20
        let titleTopGap: CGFloat = 2
        let titleBotGap: CGFloat = 2
        let contentH = frame.height - titleH - titleTopGap - titleBotGap - 4

        let content = NSView(frame: NSRect(x: 6, y: 4,
                                           width: frame.width - 12,
                                           height: contentH))
        content.autoresizingMask = [.width, .height]
        card.addSubview(content)

        // Title added after content so it sits on top in Z-order as
        // belt-and-suspenders for any future layout fiddling.
        let titleLbl = NSTextField(labelWithString: title)
        titleLbl.font = NSFont.boldSystemFont(ofSize: 12)
        titleLbl.textColor = .secondaryLabelColor
        titleLbl.frame = NSRect(x: 10,
                                y: frame.height - titleH - titleTopGap,
                                width: frame.width - 20, height: titleH)
        titleLbl.autoresizingMask = [.minYMargin, .width]
        card.addSubview(titleLbl)

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

        // Tooltips describe each register in terms of what's stored and
        // how the CPU uses it — shown on hover over either the label or
        // the value field.
        let leftRows: [(label: String, hex: Int, tip: String)] = [
            ("P",  2, "Processor Status Register — holds the 9-bit " +
             "flag word. Use the flag strip below to read individual " +
             "bits (N V M X D I Z C + E)."),
            ("A",  4, "Accumulator — primary arithmetic register. " +
             "Width depends on the M flag: 8-bit when M=1, 16-bit " +
             "when M=0 (native mode only)."),
            ("X",  4, "X index register. Width depends on the X " +
             "flag: 8-bit when X=1, 16-bit when X=0. Emulation mode " +
             "forces X=1."),
            ("Y",  4, "Y index register. Shares the X flag with the " +
             "X register for sizing."),
            ("DP", 4, "Direct Page register — 16-bit base added to " +
             "8-bit direct-page operands. Apple II / emulation mode " +
             "keeps DP=0 so direct page = zero page."),
            ("M",  2, "IIgs Machine State ($C068) — live I/O " +
             "softswitch image. Not a CPU register; shown here for " +
             "convenience. Bits: 80STORE, RDCXROM, RAMRD, RAMWRT, " +
             "LCBANK, LCREAD, LCWRITE, INTCXROM."),
        ]
        let rightRows: [(label: String, hex: Int, tip: String)] = [
            ("PC", 6, "Program Counter (24-bit) — high byte is K " +
             "(Program Bank Register), low 16 bits are the address " +
             "within the bank."),
            ("K",  2, "Program Bank Register (PBR) — bank portion " +
             "of PC. JMP / JSR within-bank don't change it; JML / " +
             "JSL do."),
            ("B",  2, "Data Bank Register (DBR) — default bank for " +
             "data memory accesses (LDA abs, STA abs, etc.). Set " +
             "via PLB / PHB / PEA manipulations."),
            ("SP", 4, "Stack Pointer — 16-bit in native mode (stack " +
             "can span any bank-0 address). In emulation mode (E=1) " +
             "the hardware forces stack operations to $01xx on the " +
             "address bus, so this field shows $01 + low byte even " +
             "if the register itself hasn't been flushed yet after " +
             "an XCE."),
            ("Q",  2, "Composite state — bit 7 is $C036 speed / " +
             "shadow-all; bits 6..0 mirror $C035 (shadow register " +
             "per-region inhibit bits)."),
            ("S",  2, "Shadow register ($E0C035). Per-region " +
             "shadow inhibit bits (1 = shadow disabled): " +
             "b0=TextPage1, b1=HiRes1, b2=HiRes2, b3=SuperHiRes " +
             "($E1 $2000-$9FFF), b4=AuxHiRes ($01), b5=reserved, " +
             "b6=I/O + LC ($Cxxx, $D000-$FFFF), b7=all-shadow."),
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
                      editable: Bool, tip: String) -> NSTextField {
            let lbl = NSTextField(labelWithString: "\(label):")
            lbl.font = font
            lbl.alignment = .right
            lbl.textColor = .secondaryLabelColor
            lbl.frame = NSRect(x: x, y: y + 2, width: wLabel, height: rowH - 4)
            lbl.toolTip = tip
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
            tf.toolTip          = tip
            content.addSubview(tf)
            return tf
        }

        // Left column — 6 rows starting at top
        var y = firstRowBottomY
        for row in leftRows {
            let editable = row.label != "M"
            regFields[row.label] = addField(label: row.label,
                                            hexDigits: row.hex,
                                            x: 6, y: y,
                                            wLabel: 28, wField: 70,
                                            editable: editable,
                                            tip: row.tip)
            y -= rowH
        }

        // Right column — 5 rows, starting at top aligned with left
        y = firstRowBottomY
        let rxLabel: CGFloat = 170
        for row in rightRows {
            // Q and S are pseudo-registers (live softswitch images),
            // not directly editable by the user.
            let editable = row.label != "Q" && row.label != "S"
            // Binary-display fields (Q, S) need enough width for 8 chars.
            let wField: CGFloat
            if row.hex == 6 { wField = 70 }
            else if row.hex == 4 { wField = 58 }
            else if row.label == "Q" || row.label == "S" { wField = 70 }
            else { wField = 34 }
            regFields[row.label] = addField(label: row.label,
                                            hexDigits: row.hex,
                                            x: rxLabel, y: y,
                                            wLabel: 28, wField: wField,
                                            editable: editable,
                                            tip: row.tip)
            y -= rowH
        }

        // Flag-letter strip: full-width row at the bottom of the card.
        // Tooltips name each bit and describe when it's meaningful.
        let flagY: CGFloat = 2
        // Order: E M X D I Z C N V — mode flags first (E gates the
        // meaning of M/X), then the rest of the control bits, with
        // condition flags (N, V) pushed to the right.
        let flagInfo: [(name: String, tip: String)] = [
            ("E", "Emulation mode. 1 = 6502 emulation (M and X forced " +
             "to 1, stack confined to $01xx). 0 = native 65816. " +
             "Toggled via the XCE instruction (swaps E with Carry)."),
            ("M", "Accumulator / Memory width. 1 = 8-bit, 0 = 16-bit. " +
             "Meaningful only in native mode (emulation forces M=1). " +
             "Toggled by REP / SEP."),
            ("X", "Index register width (X and Y). 1 = 8-bit, 0 = " +
             "16-bit. Meaningful only in native mode. Toggled by " +
             "REP / SEP."),
            ("D", "Decimal mode — ADC / SBC operate in BCD when set."),
            ("I", "IRQ disable — IRQ lines are ignored when set. " +
             "CLI enables, SEI disables."),
            ("Z", "Zero — set when the last result was zero."),
            ("C", "Carry — unsigned carry / borrow from the last " +
             "add or subtract. Also used by shifts (ASL / LSR / " +
             "ROL / ROR) and CMP."),
            ("N", "Negative — bit 7 (or bit 15 in 16-bit mode) of " +
             "the last result. Set when the high bit is 1."),
            ("V", "Overflow — signed overflow from the last ADC / " +
             "SBC / BIT. Also cleared by CLV."),
        ]
        let flagStride = (content.bounds.width - 20) /
                         CGFloat(flagInfo.count)
        for (i, f) in flagInfo.enumerated() {
            let lb = NSTextField(labelWithString: f.name)
            lb.font = NSFont.monospacedSystemFont(ofSize: 13,
                                                   weight: .bold)
            lb.alignment = .center
            lb.frame = NSRect(
                x: 10 + CGFloat(i) * flagStride,
                y: flagY,
                width: flagStride, height: flagsRowH)
            lb.toolTip = f.tip
            content.addSubview(lb)
            flagLabels[f.name] = lb
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

        // PSym = symbol for the instruction's PC (program symbol).
        // TSym = symbol for the address the instruction targets
        //        (data ref or branch/jump destination).
        // M/X and Cyc are gone: M/X duplicates the Registers panel, and
        // the cycle count was only ever a rough estimate.
        let colSpecs: [(String, CGFloat)] = [
            ("Address / Bytes", 210),
            ("Disassembly", 260),
            ("PSym", 280),
            ("TSym", 280),
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
            // On the running→halted edge, refresh symbol bindings so
            // the disassembly's PSym/TSym columns and the resolved
            // register view see whatever the program has loaded since
            // the last scan. The throttled version runs inside WDM
            // traps; this edge-triggered one is unconditional.
            if halted {
                symbols_scan_and_bind()
            }
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

        // Reflect run state in the window title instead of a toolbar
        // label. Stepping shows as a finer-grained halted sub-state.
        let stateTag: String
        if halted {
            stateTag = regs.stepping != 0 ? "Halted (stepping)" : "Halted"
        } else {
            stateTag = "Running"
        }
        window?.title = "GSplus Debugger Tracer [\(stateTag)]"

        // Always refresh panels so they display a snapshot regardless of
        // run state. The window title indicates whether values are
        // live (halted) or the last captured state (running).
        refreshRegisters(regs: regs)
        refreshStack(regs: regs)

        // Nudge the detached memory viewer to resnap if it's open. It
        // only actually redraws while the emulator is halted.
        memViewer?.notifyStateChanged(halted: halted,
                                       dp: UInt32(regs.dp))

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
        // Mirror the display-side normalization: in emulation mode the
        // effective SP is always $01xx, even if engine.stack's high
        // byte hasn't been flushed yet. Using the stale high byte
        // would have the Stack panel walking memory at $BF00+ instead
        // of page 1.
        let sp: UInt32 = nativeMode
            ? UInt32(regs.sp)
            : 0x0100 | (UInt32(regs.sp) & 0xFF)
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
        // Accumulator width follows M, index-register width follows X.
        // Emulation mode forces M=X=1, so this collapses to 8-bit for
        // all three; native mode picks per-flag.
        let aFmt = regs.flag_m8 != 0 ? "%02X" : "%04X"
        let xFmt = regs.flag_x8 != 0 ? "%02X" : "%04X"
        let aMask: UInt = regs.flag_m8 != 0 ? 0xFF : 0xFFFF
        let xMask: UInt = regs.flag_x8 != 0 ? 0xFF : 0xFFFF

        // Emulation mode (E=1) forces stack accesses to $01xx on the
        // address bus. The register itself can retain a stale high byte
        // right after an XCE — KEGS only normalizes it on the next
        // PLP/RTI/push. Display the *effective* stack pointer the CPU
        // would actually use so the card can't show a misleading
        // $BFB6-style value during emulation mode.
        let spEffective: UInt = regs.flag_e != 0
            ? (0x0100 | UInt(regs.sp) & 0xFF)
            : UInt(regs.sp)

        setField("PC", "%06X", UInt(regs.pc))
        setField("A",  aFmt,   UInt(regs.a) & aMask)
        setField("X",  xFmt,   UInt(regs.x) & xMask)
        setField("Y",  xFmt,   UInt(regs.y) & xMask)
        setField("SP", "%04X", spEffective)
        setField("DP", "%04X", UInt(regs.dp))
        setField("P",  "%02X", UInt(regs.psr & 0xFF))
        setField("K",  "%02X", UInt(regs.pbank))
        setField("B",  "%02X", UInt(regs.dbank))

        // M/Q/S shown as 8-bit binary — these are bit-flag registers,
        // so seeing individual bits is more useful than a hex byte.
        if let f = regFields["M"] { f.stringValue = binary8(regs.m_state) }
        if let f = regFields["Q"] { f.stringValue = binary8(regs.q_state) }
        if let f = regFields["S"] { f.stringValue = binary8(regs.shadow) }

        // Flag letters: bold when set, grey when unset. E is bit 8 of
        // the full 9-bit PSR (Tracer_regs.psr is UInt16), not a bit in
        // the low byte.
        let psr = Int(regs.psr)
        let bits: [(String, Int)] = [
            ("E", 0x100),
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

    /// Disassemble around `pc` so the PC row is near the middle of the
    /// list (and centered in the scroll viewport after
    /// scrollDisasmToPC runs).
    ///
    /// PC is always on a correct instruction boundary (the CPU halts
    /// between instructions) — the authoritative alignment anchor.
    /// 65C816 instructions are variable-length, so walking *backward*
    /// from PC requires trial: we test every byte offset 0..maxBackup
    /// as a starting point, decode forward using the current M/X flag
    /// state, and keep candidates whose walk produces a row whose
    /// pc == PC exactly. Dense sampling, not fixed strides — the old
    /// [48,40,32,…,0] list routinely missed the true chain.
    ///
    /// Multiple backups can land on PC by coincidence (65C816 bytecode
    /// is dense, so a mid-byte start can resync on PC after a run of
    /// "valid-looking" instructions). To pick among valid alignments,
    /// score by a coincidence-penalty: count instructions that almost
    /// never appear in real IIgs user/kernel code (BRK, COP, STP,
    /// WAI) in the pre-PC rows. The winner is (lowest penalty, then
    /// deepest preceding-row count). A single BRK outweighs a lot of
    /// extra depth — a wrong-alignment with BRK-as-"instruction" is
    /// worse than a shallow-but-clean one.
    private func rebuildDisasm(anchor: UInt32, m8: Bool, x8: Bool) {
        let count = 80
        let maxBackup: UInt32 = 40

        var bestRows:    [TracerRow] = []
        var bestPenalty: Int = Int.max
        var bestPCIdx:   Int = -1

        for backup in 0...maxBackup {
            let start = anchor &- backup
            var raw = [Tracer_dasm](repeating: Tracer_dasm(), count: count)
            let n = Int(raw.withUnsafeMutableBufferPointer { b in
                tracer_disasm(start, m8 ? 1 : 0, x8 ? 1 : 0,
                              b.baseAddress, Int32(count))
            })
            let rows = (0..<n).map { TracerRow.from(raw[$0]) }
            guard let idx = rows.firstIndex(where: { $0.pc == anchor })
                else { continue }

            // Coincidence penalty — heavy weight for BRK since $00
            // bytes are common in padding/tables and their appearance
            // in a disasm walk strongly suggests misalignment.
            var penalty = 0
            for i in 0..<idx {
                let m = rows[i].mnemonic
                if m.hasPrefix("BRK")      { penalty += 100 }
                else if m.hasPrefix("COP") { penalty +=  40 }
                else if m.hasPrefix("STP") { penalty +=  40 }
                else if m.hasPrefix("WAI") { penalty +=  40 }
            }

            let better =
                penalty <  bestPenalty ||
               (penalty == bestPenalty && idx > bestPCIdx)
            if better {
                bestRows    = rows
                bestPenalty = penalty
                bestPCIdx   = idx
            }
        }
        disasmRows = bestRows
    }

    /// Scroll the disassembly table so the PC row is vertically centered in
    /// the scroll view (rather than pinned to the top/bottom edge).
    private func scrollDisasmToPC() {
        let pc = lastRegs.pc
        guard let idx = disasmRows.firstIndex(where: { $0.pc == pc }),
              let scrollView = disasmTable.enclosingScrollView
            else { return }
        let rowRect     = disasmTable.rect(ofRow: idx)
        let visibleH    = scrollView.contentView.bounds.height
        let targetY     = rowRect.midY - visibleH / 2
        let clamped     = max(0, min(targetY,
            disasmTable.bounds.height - visibleH))
        scrollView.contentView.scroll(to: NSPoint(x: 0, y: clamped))
        scrollView.reflectScrolledClipView(scrollView.contentView)
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

    @objc private func onViewMemory(_ sender: Any?) {
        if memViewer == nil {
            memViewer = MemoryViewerWindowController()
        }
        let initialDP: UInt32 = UInt32(lastRegs.dp) & 0xFFFF
        memViewer?.show(initialAddress: initialDP)
        memViewer?.notifyStateChanged(halted: lastRegs.halted != 0,
                                       dp: initialDP)
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
    let m8:       Bool
    let x8:       Bool
    let mnemonic: String

    static func from(_ d: Tracer_dasm) -> TracerRow {
        var bytesStr = ""
        let n = Int(d.nbytes)
        let arr = [d.bytes.0, d.bytes.1, d.bytes.2, d.bytes.3]
        for i in 0..<4 {
            if i < n { bytesStr += String(format: "%02X ", arr[i]) }
            else     { bytesStr += "   " }
        }
        let mnem = withUnsafePointer(to: d.mnemonic) { tup in
            tup.withMemoryRebound(to: CChar.self, capacity: 48) {
                String(cString: $0)
            }
        }
        return TracerRow(pc: d.pc,
                         bytes: bytesStr.trimmingCharacters(in: .whitespaces),
                         m8: d.m8 != 0,
                         x8: d.x8 != 0,
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
        case "PSym":
            if let cstr = symbols_describe_pc(r.pc) {
                tf.stringValue = String(cString: cstr)
                tf.textColor = .systemPurple
            } else {
                tf.stringValue = ""
                tf.textColor = .secondaryLabelColor
            }
        case "TSym":
            if let cstr = tracer_target_symbol(r.pc,
                                               Int32(r.m8 ? 1 : 0),
                                               Int32(r.x8 ? 1 : 0)) {
                tf.stringValue = String(cString: cstr)
                tf.textColor = .systemTeal
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
    // wantsUpdateLayer was previously true, but it caused AppKit to
    // bypass draw(_:) on the view's layer, and in practice this
    // prevented plain-NSView and NSTextField subviews from rendering
    // inside the Memory View card (only views with explicit draw(_:)
    // overrides, like HexDumpView, painted pixels). Removing the
    // override falls back to the normal draw-based layer path, which
    // renders all subviews correctly.
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

// ---------------------------------------------------------------------------
// MARK: - MemoryViewerWindowController
// ---------------------------------------------------------------------------

/// Standalone window for browsing / searching emulator memory. Opened
/// from the "View Memory" toolbar button on the Debugger Tracer. Only
/// renders data while the emulator is halted — while running, it
/// freezes on the last snapshot and the title strip says so.
///
/// Pure-Swift class (not NSObject-derived) for the same Swift-4 ivar
/// layout reasons described at the top of TracerWindowController.
class MemoryViewerWindowController {

    private var window:       NSWindow?
    private var addrField:    NSTextField!
    private var lenField:     NSTextField!
    private var searchField:  NSTextField!
    private var searchMode:   NSPopUpButton!
    private var hexView:      HexDumpView!
    private var hexScroll:    NSScrollView!
    private var resultTable:  NSTableView!
    private var statusLabel:  NSTextField!

    fileprivate var results:  [UInt32] = []
    private var timer:        Timer?
    private var viewAddress:  UInt32 = 0
    private var viewLength:   Int    = 1024
    private var lastBytes:    [UInt8] = []
    private var lastHalted:   Bool   = false

    init() {
        window       = nil
        timer        = nil
        results      = []
        viewAddress  = 0
        viewLength   = 1024
        lastBytes    = []
        lastHalted   = false
    }

    // MARK: - Public API

    func show(initialAddress: UInt32) {
        if window == nil {
            build()
            viewAddress = initialAddress & 0xFFFFFF
            addrField.stringValue = String(format: "$%06X", viewAddress)
        }
        startTimer()
        window?.makeKeyAndOrderFront(nil)
        refreshDump()
    }

    /// Called by the Tracer on halt-state transitions so we can redraw
    /// immediately rather than waiting for the next tick.
    func notifyStateChanged(halted: Bool, dp: UInt32) {
        lastHalted = halted
        updateStatus()
        if halted && window?.isVisible == true {
            refreshDump()
        }
    }

    // MARK: - Window build

    private func build() {
        let winW: CGFloat = 820
        let winH: CGFloat = 560
        let rect = NSRect(x: 240, y: 180, width: winW, height: winH)
        let style: NSWindow.StyleMask = [.titled, .closable,
                                          .resizable, .miniaturizable]
        let w = NSWindow(contentRect: rect, styleMask: style,
                         backing: .buffered, defer: false)
        w.title = "GSplus Memory Viewer"
        w.isReleasedWhenClosed = false
        window = w

        NotificationCenter.default.addObserver(
            forName: NSWindow.willCloseNotification,
            object: w, queue: .main) { [weak self] _ in
            self?.stopTimer()
        }

        guard let content = w.contentView else { return }

        let barH: CGFloat = 78
        let bar = NSView(frame: NSRect(x: 0, y: winH - barH,
                                        width: winW, height: barH))
        bar.autoresizingMask = [.width, .minYMargin]
        bar.wantsLayer = true
        bar.layer?.backgroundColor = NSColor(calibratedRed: 0.93,
                                              green: 0.93, blue: 0.94,
                                              alpha: 1.0).cgColor
        content.addSubview(bar)

        let sep = NSBox(frame: NSRect(x: 0, y: 0, width: winW, height: 1))
        sep.boxType = .separator
        sep.autoresizingMask = [.width]
        bar.addSubview(sep)

        // Row 1: Address / Bytes / Go / Status
        let row1Y: CGFloat = 44
        let row2Y: CGFloat = 10
        var x: CGFloat = 12

        let addrLbl = NSTextField(labelWithString: "Address:")
        addrLbl.textColor = .secondaryLabelColor
        addrLbl.frame = NSRect(x: x, y: row1Y + 4, width: 60, height: 18)
        bar.addSubview(addrLbl); x += 62

        addrField = NSTextField(frame: NSRect(x: x, y: row1Y,
                                               width: 96, height: 24))
        addrField.font = NSFont.monospacedSystemFont(ofSize: 12,
                                                      weight: .regular)
        addrField.isEditable      = true
        addrField.isSelectable    = true
        addrField.isBezeled       = true
        addrField.drawsBackground = true
        addrField.backgroundColor = .textBackgroundColor
        addrField.stringValue     = "$000000"
        addrField.target = self
        addrField.action = #selector(onGo(_:))
        bar.addSubview(addrField); x += 102

        let lenLbl = NSTextField(labelWithString: "Bytes:")
        lenLbl.textColor = .secondaryLabelColor
        lenLbl.frame = NSRect(x: x, y: row1Y + 4, width: 44, height: 18)
        bar.addSubview(lenLbl); x += 46

        lenField = NSTextField(frame: NSRect(x: x, y: row1Y,
                                              width: 72, height: 24))
        lenField.font = NSFont.monospacedSystemFont(ofSize: 12,
                                                     weight: .regular)
        lenField.isEditable      = true
        lenField.isSelectable    = true
        lenField.isBezeled       = true
        lenField.drawsBackground = true
        lenField.backgroundColor = .textBackgroundColor
        lenField.stringValue     = "1024"
        lenField.target = self
        lenField.action = #selector(onGo(_:))
        bar.addSubview(lenField); x += 78

        let goBtn = NSButton(title: "Go", target: self,
                             action: #selector(onGo(_:)))
        goBtn.bezelStyle = .rounded
        goBtn.frame = NSRect(x: x, y: row1Y - 1, width: 54, height: 26)
        bar.addSubview(goBtn); x += 60

        // Status label (run/halt state) pinned to the right edge.
        let statusW: CGFloat = 280
        statusLabel = NSTextField(labelWithString: "")
        statusLabel.alignment = .right
        statusLabel.font = NSFont.systemFont(ofSize: 12, weight: .semibold)
        statusLabel.textColor = .secondaryLabelColor
        statusLabel.frame = NSRect(x: winW - statusW - 12, y: row1Y + 4,
                                    width: statusW, height: 20)
        statusLabel.autoresizingMask = [.minXMargin]
        bar.addSubview(statusLabel)

        // Row 2: Search
        x = 12
        let searchLbl = NSTextField(labelWithString: "Search:")
        searchLbl.textColor = .secondaryLabelColor
        searchLbl.frame = NSRect(x: x, y: row2Y + 4, width: 60, height: 18)
        bar.addSubview(searchLbl); x += 62

        searchField = NSTextField(frame: NSRect(x: x, y: row2Y,
                                                 width: 340, height: 24))
        searchField.font = NSFont.monospacedSystemFont(ofSize: 12,
                                                        weight: .regular)
        searchField.isEditable      = true
        searchField.isSelectable    = true
        searchField.isBezeled       = true
        searchField.drawsBackground = true
        searchField.backgroundColor = .textBackgroundColor
        searchField.placeholderString = "text, or hex bytes: AA BB 10"
        searchField.target = self
        searchField.action = #selector(onSearch(_:))
        bar.addSubview(searchField); x += 346

        searchMode = NSPopUpButton(frame: NSRect(x: x, y: row2Y - 1,
                                                  width: 110, height: 26))
        searchMode.addItem(withTitle: "ASCII")
        searchMode.addItem(withTitle: "ASCII (hi-bit)")
        searchMode.addItem(withTitle: "Hex bytes")
        bar.addSubview(searchMode); x += 116

        let searchBtn = NSButton(title: "Search", target: self,
                                  action: #selector(onSearch(_:)))
        searchBtn.bezelStyle = .rounded
        searchBtn.frame = NSRect(x: x, y: row2Y - 1, width: 74, height: 26)
        bar.addSubview(searchBtn); x += 80

        let clearBtn = NSButton(title: "Clear", target: self,
                                 action: #selector(onClearResults(_:)))
        clearBtn.bezelStyle = .rounded
        clearBtn.frame = NSRect(x: x, y: row2Y - 1, width: 62, height: 26)
        bar.addSubview(clearBtn)

        // Bottom: hex dump (left) + results list (right).
        let resultsW: CGFloat = 200
        let bottomH = winH - barH
        let dumpRect = NSRect(x: 0, y: 0,
                              width: winW - resultsW, height: bottomH)
        hexScroll = NSScrollView(frame: dumpRect)
        hexScroll.autoresizingMask = [.width, .height]
        hexScroll.hasVerticalScroller   = true
        hexScroll.hasHorizontalScroller = false
        hexScroll.borderType            = .noBorder
        content.addSubview(hexScroll)

        hexView = HexDumpView(frame: NSRect(x: 0, y: 0,
                                              width: dumpRect.width,
                                              height: bottomH))
        hexView.autoresizingMask = [.width]
        hexScroll.documentView = hexView

        let tblRect = NSRect(x: winW - resultsW, y: 0,
                              width: resultsW, height: bottomH)
        let tblScroll = NSScrollView(frame: tblRect)
        tblScroll.autoresizingMask = [.minXMargin, .height]
        tblScroll.hasVerticalScroller = true
        tblScroll.borderType          = .bezelBorder
        content.addSubview(tblScroll)

        let tv = NSTableView(frame: tblScroll.bounds)
        tv.autoresizingMask   = [.width, .height]
        tv.rowSizeStyle       = .small
        tv.rowHeight          = 16
        tv.gridStyleMask      = []
        tv.headerView         = NSTableHeaderView()
        tv.backgroundColor    = .controlBackgroundColor
        let col = NSTableColumn(
            identifier: NSUserInterfaceItemIdentifier("addr"))
        col.title = "Matches"
        col.width = resultsW - 24
        tv.addTableColumn(col)
        MemResultsBridge.shared.owner = self
        tv.dataSource = MemResultsBridge.shared
        tv.delegate   = MemResultsBridge.shared
        tv.target     = self
        tv.action     = #selector(onResultClicked(_:))
        tblScroll.documentView = tv
        resultTable = tv
    }

    // MARK: - Timer

    private func startTimer() {
        stopTimer()
        timer = Timer.scheduledTimer(withTimeInterval: 0.5,
                                      repeats: true) { [weak self] _ in
            self?.tick()
        }
    }

    private func stopTimer() {
        timer?.invalidate()
        timer = nil
    }

    private func tick() {
        let halted = tracer_is_halted() != 0
        if halted != lastHalted {
            lastHalted = halted
            updateStatus()
        }
        // Only refresh from live memory while halted — keeps the view
        // stable while the CPU is running (per the user's spec).
        if halted { refreshDump() }
    }

    // MARK: - Actions

    @objc private func onGo(_ sender: Any?) {
        guard let addr = parseAddress(addrField.stringValue) else {
            NSSound.beep(); return
        }
        let raw = lenField.stringValue.trimmingCharacters(in: .whitespaces)
        if let n = parseInt(raw), n > 0 {
            viewLength = min(max(n, 16), 0x10000)  // cap at 64K per view
            lenField.stringValue = "\(viewLength)"
        }
        viewAddress = addr & 0xFFFFFF
        addrField.stringValue = String(format: "$%06X", viewAddress)
        refreshDump()
    }

    @objc private func onSearch(_ sender: Any?) {
        let bytes = searchBytesFromInput()
        guard !bytes.isEmpty else {
            results = []
            resultTable?.reloadData()
            return
        }
        results = findAll(pattern: bytes, maxResults: 512)
        resultTable?.reloadData()
        if let first = results.first {
            viewAddress = first & 0xFFFFFF
            addrField.stringValue = String(format: "$%06X", viewAddress)
            refreshDump()
        }
    }

    @objc private func onClearResults(_ sender: Any?) {
        results = []
        resultTable?.reloadData()
        hexView?.setHighlights([])
    }

    @objc fileprivate func onResultClicked(_ sender: Any?) {
        let row = resultTable.clickedRow
        guard row >= 0 && row < results.count else { return }
        let hit = results[row]
        // Center the hit a few rows into the new window.
        let pre: UInt32 = 48
        let start: UInt32 = hit >= pre ? (hit - pre) & 0xFFFFF0 : 0
        viewAddress = start
        addrField.stringValue = String(format: "$%06X", viewAddress)
        refreshDump()
    }

    // MARK: - Read + render

    private func refreshDump() {
        // Only read and redraw while halted — otherwise the view could
        // flicker from per-instruction memory churn while running.
        if tracer_is_halted() == 0 {
            updateStatus()
            return
        }

        var buf = [UInt8](repeating: 0, count: viewLength)
        buf.withUnsafeMutableBufferPointer { b in
            tracer_read_memory(viewAddress, b.baseAddress, Int32(viewLength))
        }
        lastBytes = buf
        hexView.setData(address: viewAddress, bytes: buf)

        // Paint any search hits that fall inside this window.
        var hl: [HexHighlight] = []
        if !results.isEmpty {
            let bytes = searchBytesFromInput()
            let width = max(bytes.count, 1)
            for hit in results {
                let off = Int(Int64(hit) - Int64(viewAddress))
                if off >= 0 && off + width <= buf.count {
                    hl.append(HexHighlight(
                        byteOffset: off, byteCount: width,
                        color: NSColor.systemYellow.withAlphaComponent(0.55)))
                }
            }
        }
        hexView.setHighlights(hl)

        // Resize the hex view to fit its rows so the scroll view scrolls.
        let rows = (viewLength + 15) / 16
        let contentH = CGFloat(rows) * 14 + 8
        var f = hexView.frame
        f.size.height = max(contentH, hexScroll.contentSize.height)
        hexView.frame = f

        updateStatus()
    }

    private func updateStatus() {
        let halted = lastHalted
        statusLabel.stringValue = halted
            ? "Halted — live view"
            : "Running — paused (last snapshot)"
        statusLabel.textColor = halted ? .systemGreen : .systemOrange
    }

    // MARK: - Search helpers

    /// Translates the search field + mode selector into a concrete byte
    /// pattern. Returns empty on unparseable input.
    fileprivate func searchBytesFromInput() -> [UInt8] {
        let s = searchField.stringValue
        if s.isEmpty { return [] }

        switch searchMode.indexOfSelectedItem {
        case 0:  // ASCII
            return Array(s.utf8)
        case 1:  // ASCII (hi-bit, Apple-style)
            return Array(s.utf8).map { $0 | 0x80 }
        default: // Hex bytes
            var out: [UInt8] = []
            let toks = s.split(whereSeparator: { " ,\t".contains($0) })
            for t in toks {
                var str = String(t)
                if str.hasPrefix("$")  { str.removeFirst() }
                if str.hasPrefix("0x") || str.hasPrefix("0X") {
                    str = String(str.dropFirst(2))
                }
                if str.count == 0 || str.count > 2 { return [] }
                guard let v = UInt8(str, radix: 16) else { return [] }
                out.append(v)
            }
            return out
        }
    }

    /// Linear full-address-space scan for `pattern`. Reads in 4 KB
    /// chunks with a `pattern.count - 1` byte overlap so we don't miss
    /// matches that straddle chunk boundaries.
    private func findAll(pattern: [UInt8], maxResults: Int) -> [UInt32] {
        if pattern.isEmpty { return [] }
        var hits: [UInt32] = []
        let chunk = 4096
        let overlap = max(pattern.count - 1, 0)
        var addr: UInt64 = 0
        let end: UInt64 = 0x1000000   // 24-bit space
        var buf = [UInt8](repeating: 0, count: chunk + overlap)
        while addr < end {
            let remaining = Int(min(UInt64(chunk + overlap), end - addr))
            buf.withUnsafeMutableBufferPointer { b in
                tracer_read_memory(UInt32(addr), b.baseAddress, Int32(remaining))
            }
            // Scan within the non-overlap region; overlap only needs to
            // be present for boundary-straddling matches.
            let scanLimit = Int(min(UInt64(chunk), end - addr))
            var i = 0
            while i < scanLimit {
                if i + pattern.count > remaining { break }
                var match = true
                for j in 0..<pattern.count {
                    if buf[i + j] != pattern[j] { match = false; break }
                }
                if match {
                    hits.append(UInt32(addr) &+ UInt32(i))
                    if hits.count >= maxResults { return hits }
                }
                i += 1
            }
            addr += UInt64(chunk)
        }
        return hits
    }

    fileprivate func parseAddress(_ s: String) -> UInt32? {
        if let v = parseInt(s) { return UInt32(v & 0xFFFFFF) }
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
}

// ---------------------------------------------------------------------------
// MARK: - MemResultsBridge (NSTableView data source)
// ---------------------------------------------------------------------------

fileprivate class MemResultsBridge: NSObject,
    NSTableViewDataSource, NSTableViewDelegate {

    static let shared = MemResultsBridge()
    weak var owner: AnyObject?

    func numberOfRows(in tableView: NSTableView) -> Int {
        return (owner as? MemoryViewerWindowController)?.results.count ?? 0
    }

    func tableView(_ tv: NSTableView, viewFor tableColumn: NSTableColumn?,
                   row: Int) -> NSView? {
        let id = NSUserInterfaceItemIdentifier("memResultCell")
        let cell = tv.makeView(withIdentifier: id, owner: nil) as? NSTableCellView
                   ?? NSTableCellView()
        if cell.identifier == nil { cell.identifier = id }
        let tf: NSTextField
        if let existing = cell.textField {
            tf = existing
        } else {
            tf = NSTextField(labelWithString: "")
            tf.font = NSFont.monospacedSystemFont(ofSize: 11, weight: .regular)
            tf.translatesAutoresizingMaskIntoConstraints = false
            cell.addSubview(tf)
            cell.textField = tf
            NSLayoutConstraint.activate([
                tf.leadingAnchor.constraint(equalTo: cell.leadingAnchor,
                                             constant: 4),
                tf.centerYAnchor.constraint(equalTo: cell.centerYAnchor),
                tf.trailingAnchor.constraint(equalTo: cell.trailingAnchor,
                                              constant: -4),
            ])
        }
        if let owner = owner as? MemoryViewerWindowController,
           row >= 0 && row < owner.results.count {
            tf.stringValue = String(format: "$%06X", owner.results[row])
        } else {
            tf.stringValue = ""
        }
        return cell
    }
}
