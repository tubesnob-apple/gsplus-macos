/**********************************************************************/
/*                    GSplus - Apple //gs Emulator                    */
/*                    Copyright 2025-2026 GSplus Contributors         */
/*                                                                    */
/*      This code is covered by the GNU GPL v3                        */
/*      See the file COPYING.txt or https://www.gnu.org/licenses/     */
/**********************************************************************/

import Cocoa

// Shell-style debug console (F8).
//
// A single NSTextView handles both output and input — no separate input
// field.  Output is spliced in above the current prompt line so the
// prompt always sits at the bottom, just like a REPL or terminal.
//
// promptMark: character index in textStorage where the current prompt
//             line begins.  Everything from promptMark to the end is
//             the "live tail" — prompt string + whatever the user has
//             typed so far.  Everything before it is immutable history.

// ---------------------------------------------------------------------------
// MARK: - DebugTextView
// ---------------------------------------------------------------------------

class DebugTextView: NSTextView {

    // Set by DebugConsoleWindowController after construction
    weak var console: DebugConsoleWindowController?

    override var acceptsFirstResponder: Bool { return true }

    // We handle ALL keystrokes manually so the shell behaviour is
    // consistent.  Do NOT call super.keyDown — that would let NSTextView
    // try to insert/delete text on its own.
    override func keyDown(with event: NSEvent) {
        guard let c = console else { return }
        let mods    = event.modifierFlags
        let keyCode = event.keyCode
        let chars   = event.characters ?? ""

        // Ctrl-C — cancel current input line
        if mods.contains(.control) && keyCode == 8 {   // 8 = 'C' key
            c.handleCtrlC()
            return
        }

        switch keyCode {
        case 36, 76:                    // Return / numpad Enter
            c.handleReturn()
        case 51:                        // Backspace
            c.handleBackspace()
        case 126:                       // Up arrow  → older history
            c.handleHistory(up: true)
        case 125:                       // Down arrow → newer history
            c.handleHistory(up: false)
        default:
            // Accept printable ASCII not modified by Cmd/Ctrl
            if !chars.isEmpty &&
               !mods.contains(.command) &&
               !mods.contains(.control) {
                for ch in chars {
                    let v = ch.unicodeScalars.first!.value
                    if v >= 0x20 && v < 0x7f {
                        c.handleChar(String(ch))
                    }
                }
            }
        }
    }

    // Let Cmd-C (copy selection) and Cmd-A (select all) pass through
    // to NSTextView's built-in handlers.
    override func performKeyEquivalent(with event: NSEvent) -> Bool {
        if event.modifierFlags.contains(.command) {
            switch event.characters {
            case "c": copy(nil);      return true
            case "a": selectAll(nil); return true
            default:  break
            }
        }
        return false
    }
}

// ---------------------------------------------------------------------------
// MARK: - DebugConsoleWindowController
// ---------------------------------------------------------------------------

// Pure Swift class — intentionally does NOT inherit from NSObject.
// NSObject subclasses with many Swift stored properties have unreliable
// ivar layout in Swift 4 per-file compilation mode, causing ARC crashes.
// Window/scroll events are handled via block-based NotificationCenter
// observers instead of NSWindowDelegate / @objc selectors.

class DebugConsoleWindowController {

    // MARK: Subviews
    private var window:     NSWindow?
    private var textView:   DebugTextView!
    private var scrollView: NSScrollView!

    // MARK: Shell state
    // Index in textStorage where the current prompt line starts.
    // textStorage[promptMark...] == promptString + inputBuffer  (always)
    private var promptMark:    Int    = 0
    private let promptString:  String = "> "

    private var inputBuffer:   String = ""
    private var commandHistory: [String] = []
    private var historyIndex:  Int    = -1   // -1 = not browsing history
    private var savedInput:    String = ""   // preserved while browsing history

    // MARK: Display state
    private var lastSeenTotal: Int  = 0
    private var autoScroll:    Bool = true
    // Guard flag: skip scroll-observer updates while we're splicing text
    private var isSplicing:    Bool = false

    // MARK: Text attributes
    private let outputFont = NSFont(name: "Menlo", size: 12)
                             ?? NSFont.systemFont(ofSize: 12)
    private var outputAttrs: [NSAttributedString.Key: Any] {
        return [
            .font:            outputFont,
            .foregroundColor: NSColor(calibratedWhite: 0.85, alpha: 1.0),
        ]
    }
    private var inputAttrs: [NSAttributedString.Key: Any] {
        return [
            .font:            outputFont,
            .foregroundColor: NSColor(calibratedRed: 0.4,
                                      green:         0.9,
                                      blue:          0.4,
                                      alpha:         1.0),
        ]
    }

    var isOpen: Bool { return window?.isVisible ?? false }

    init() {
        window        = nil
        textView      = nil
        scrollView    = nil
        promptMark    = 0
        inputBuffer   = ""
        commandHistory = []
        historyIndex  = -1
        savedInput    = ""
        lastSeenTotal = 0
        autoScroll    = true
        isSplicing    = false
    }

    // MARK: - Public API (called by AppDelegate)

    func toggle() {
        if window == nil { createWindow() }
        guard let w = window else { return }
        if w.isVisible {
            w.orderOut(nil)
        } else {
            syncAllLines()
            w.makeKeyAndOrderFront(nil)
            w.makeFirstResponder(textView)
        }
    }

    // Called every emulator tick — splices any new ring-buffer lines in
    // above the current prompt.  Also auto-opens on emulator halt.
    func updateIfNeeded() {
        if !isOpen && g_halt_sim > 0 {
            if window == nil { createWindow() }
            syncAllLines()
            window?.makeKeyAndOrderFront(nil)
            window?.makeFirstResponder(textView)
        }
        guard isOpen else { return }
        let current = Int(g_debug_lines_total)
        guard current != lastSeenTotal else { return }
        let pos   = Int(g_debug_lines_pos)
        let alloc = Int(g_debug_lines_alloc)
        var added = current - lastSeenTotal
        if added < 0 || added > alloc { added = alloc }
        lastSeenTotal = current
        let text = collectLines(count: added, writePos: pos, alloc: alloc)
        if !text.isEmpty {
            spliceOutput(text)
        }
    }

    // MARK: - Keystroke handlers (called by DebugTextView)

    func handleChar(_ s: String) {
        guard let storage = textView.textStorage else { return }
        inputBuffer += s
        storage.append(NSAttributedString(string: s, attributes: inputAttrs))
        scrollIfNeeded()
    }

    func handleBackspace() {
        guard !inputBuffer.isEmpty,
              let storage = textView.textStorage else { return }
        inputBuffer.removeLast()
        let len = storage.length
        if len > 0 {
            storage.deleteCharacters(in: NSRange(location: len - 1, length: 1))
        }
        scrollIfNeeded()
    }

    func handleReturn() {
        guard let storage = textView.textStorage else { return }
        let cmd = inputBuffer.trimmingCharacters(in: .whitespaces)

        // Append newline to close the input line visually
        storage.append(NSAttributedString(string: "\n", attributes: inputAttrs))

        // Record history (skip duplicates and blanks)
        if !cmd.isEmpty &&
           (commandHistory.isEmpty || commandHistory.last != cmd) {
            commandHistory.append(cmd)
        }
        historyIndex = -1
        savedInput   = ""
        inputBuffer  = ""

        // promptMark moves to end of storage; new output from the command
        // will be picked up by updateIfNeeded on the next tick and spliced
        // in before the next prompt.
        promptMark = storage.length

        // Execute — output lands in the ring buffer synchronously, but
        // updateIfNeeded delivers it on the next main_run_loop tick.
        if !cmd.isEmpty {
            do_debug_cmd(cmd)
        }

        // Show new prompt immediately
        appendPrompt()
        scrollIfNeeded()
    }

    func handleHistory(up: Bool) {
        guard !commandHistory.isEmpty else { return }
        if up {
            if historyIndex == -1 {
                savedInput   = inputBuffer
                historyIndex = commandHistory.count - 1
            } else if historyIndex > 0 {
                historyIndex -= 1
            } else {
                return  // already at oldest
            }
            replaceInputWith(commandHistory[historyIndex])
        } else {
            guard historyIndex != -1 else { return }
            historyIndex += 1
            if historyIndex >= commandHistory.count {
                historyIndex = -1
                replaceInputWith(savedInput)
            } else {
                replaceInputWith(commandHistory[historyIndex])
            }
        }
    }

    func handleCtrlC() {
        guard let storage = textView.textStorage else { return }
        // Erase everything from promptMark to end, reset state, new prompt
        let tailLen = storage.length - promptMark
        if tailLen > 0 {
            storage.deleteCharacters(
                in: NSRange(location: promptMark, length: tailLen))
        }
        inputBuffer  = ""
        historyIndex = -1
        storage.append(NSAttributedString(string: "^C\n", attributes: outputAttrs))
        promptMark = storage.length
        appendPrompt()
        scrollIfNeeded()
    }

    // MARK: - Window construction

    private func createWindow() {
        let rect  = NSRect(x: 100, y: 100, width: 900, height: 550)
        let style: NSWindow.StyleMask = [.titled, .closable,
                                          .resizable, .miniaturizable]
        let w = NSWindow(contentRect: rect,
                         styleMask: style,
                         backing: .buffered,
                         defer: false)
        w.title              = "GSplus Debug Console"
        w.isReleasedWhenClosed = false
        buildLayout(in: w)

        // Window lifecycle notifications (block-based — no NSWindowDelegate needed)
        NotificationCenter.default.addObserver(
            forName: NSWindow.willCloseNotification,
            object: w, queue: .main) { [weak self] _ in
            self?.autoScroll = true
        }
        NotificationCenter.default.addObserver(
            forName: NSWindow.didBecomeKeyNotification,
            object: w, queue: .main) { [weak self] _ in
            guard let s = self else { return }
            s.window?.makeFirstResponder(s.textView)
        }

        window = w
    }

    private func buildLayout(in w: NSWindow) {
        guard let content = w.contentView else { return }

        let sv = NSScrollView(frame: content.bounds)
        sv.autoresizingMask      = [.width, .height]
        sv.hasVerticalScroller   = true
        sv.hasHorizontalScroller = false
        sv.autohidesScrollers    = false

        let tv = DebugTextView(frame:
            NSRect(x: 0, y: 0,
                   width: sv.contentSize.width,
                   height: sv.contentSize.height))
        tv.console                = self
        tv.isEditable             = false   // we manage editing in keyDown
        tv.isSelectable           = true
        tv.autoresizingMask       = [.width]
        tv.backgroundColor        = NSColor(calibratedRed: 0.08,
                                             green: 0.08,
                                             blue:  0.10,
                                             alpha: 1.0)
        tv.textContainerInset     = NSSize(width: 4, height: 4)
        tv.usesFontPanel          = false

        sv.documentView = tv
        content.addSubview(sv)
        scrollView = sv
        textView   = tv

        // Track scroll position to disable auto-scroll while user reads history
        sv.contentView.postsBoundsChangedNotifications = true
        NotificationCenter.default.addObserver(
            forName: NSView.boundsDidChangeNotification,
            object: sv.contentView, queue: .main) { [weak self] _ in
            self?.handleScrollBoundsChanged()
        }
    }

    // MARK: - Scroll tracking

    private func handleScrollBoundsChanged() {
        guard !isSplicing,
              let sv      = scrollView,
              let docView = sv.documentView else { return }
        let clipY = sv.contentView.bounds.origin.y
        let maxY  = docView.frame.height - sv.contentView.bounds.height
        autoScroll = (clipY >= maxY - 20)
    }

    private func scrollIfNeeded() {
        if autoScroll { textView.scrollToEndOfDocument(nil) }
    }

    // MARK: - Text / prompt helpers

    // Appends the prompt string at end of storage.
    // promptMark must already be set to storage.length before calling.
    private func appendPrompt() {
        guard let storage = textView.textStorage else { return }
        storage.append(
            NSAttributedString(string: promptString, attributes: inputAttrs))
    }

    // Replaces the text after the prompt string with newInput.
    private func replaceInputWith(_ newInput: String) {
        guard let storage = textView.textStorage else { return }
        let inputStart = promptMark + promptString.count
        let inputLen   = storage.length - inputStart
        if inputLen > 0 {
            storage.deleteCharacters(
                in: NSRange(location: inputStart, length: inputLen))
        }
        inputBuffer = newInput
        if !newInput.isEmpty {
            storage.append(
                NSAttributedString(string: newInput, attributes: inputAttrs))
        }
        scrollIfNeeded()
    }

    // MARK: - Output splicing

    // Inserts `text` (already newline-terminated) above the current
    // prompt line, then restores prompt + inputBuffer at the new position.
    private func spliceOutput(_ text: String) {
        guard let storage = textView.textStorage else { return }

        isSplicing = true
        storage.beginEditing()

        let tailLen = storage.length - promptMark
        if tailLen > 0 {
            storage.deleteCharacters(
                in: NSRange(location: promptMark, length: tailLen))
        }

        storage.append(NSAttributedString(string: text, attributes: outputAttrs))
        promptMark = storage.length

        // Restore prompt + whatever the user had typed
        let tail = promptString + inputBuffer
        storage.append(NSAttributedString(string: tail, attributes: inputAttrs))

        storage.endEditing()
        isSplicing = false

        if autoScroll { textView.scrollToEndOfDocument(nil) }
    }

    // MARK: - Ring-buffer reading

    // Replays up to 20,000 lines on window open, sets initial prompt.
    private func syncAllLines() {
        guard let storage = textView.textStorage else { return }
        storage.setAttributedString(NSAttributedString())
        inputBuffer  = ""
        historyIndex = -1
        promptMark   = 0

        let total = Int(g_debug_lines_total)
        let pos   = Int(g_debug_lines_pos)
        let alloc = Int(g_debug_lines_alloc)
        lastSeenTotal = total

        let count = min(total, min(alloc, 20000))
        let text  = collectLines(count: count, writePos: pos, alloc: alloc)
        if !text.isEmpty {
            storage.append(NSAttributedString(string: text, attributes: outputAttrs))
        }

        promptMark = storage.length
        appendPrompt()
        textView.scrollToEndOfDocument(nil)
    }

    // Collects `count` most-recently-written ring-buffer lines, oldest first.
    // Returns a single newline-terminated string ready to insert.
    private func collectLines(count: Int, writePos: Int, alloc: Int) -> String {
        guard count > 0,
              let ptr = g_debug_lines_ptr,
              alloc > 0 else { return "" }
        var out = ""
        for back in stride(from: count - 1, through: 0, by: -1) {
            var idx = writePos - 1 - back
            if idx < 0 { idx += alloc }
            guard idx >= 0 && idx < alloc else { continue }
            out += decodeLine(ptr.advanced(by: idx)) + "\n"
        }
        return out
    }

    // Decodes one 80-byte Apple II ring-buffer entry to a plain String.
    // Bytes are stored XOR'd with 0x80; undo that and trim trailing spaces.
    private func decodeLine(_ entry: UnsafeMutablePointer<Debug_entry>) -> String {
        let maxChars = Int(DEBUG_ENTRY_MAX_CHARS)
        var bytes    = [UInt8](repeating: 0x20, count: maxChars)
        withUnsafeBytes(of: &entry.pointee.str_buf) { raw in
            for i in 0 ..< min(raw.count, maxChars) {
                let b = raw[i] ^ 0x80
                bytes[i] = (b >= 0x20 && b < 0x7f) ? b : 0x20
            }
        }
        var end = maxChars
        while end > 0 && bytes[end - 1] == 0x20 { end -= 1 }
        return String(bytes: bytes.prefix(end), encoding: .ascii) ?? ""
    }
}
