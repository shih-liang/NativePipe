import AppKit
import NativePipeProtocol
import SwiftUI

/// Shared by shortcut remapping and global shortcuts. The caller supplies any
/// scope-specific validation; key choices, recording and labels stay identical.
public struct ShortcutChordEditor: View {
    public static let recordingDidChange = Notification.Name("NativePipe.ShortcutRecordingChanged")
    let title: String
    @Binding var chord: ShortcutChord
    let mac: Bool
    let validate: (ShortcutChord) -> String?
    @State private var error: String?

    public init(title: String, chord: Binding<ShortcutChord>, mac: Bool,
                validate: @escaping (ShortcutChord) -> String? = { _ in nil }) {
        self.title = title; _chord = chord; self.mac = mac; self.validate = validate
    }

    public var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(alignment: .top) {
                Text(title)
                Spacer(minLength: 16)
                VStack(alignment: .trailing, spacing: 8) {
                    if mac {
                        ShortcutRecorder(chord: chord, onChange: change, onError: { error = $0 })
                            .frame(width: 180, height: 26)
                    }
                    Picker("Key", selection: Binding(get: { chord.key }, set: { key in
                        var next = chord; next.key = key; _ = change(next)
                    })) {
                        ForEach(ShortcutKey.allCases, id: \.self) { Text($0.title).tag($0) }
                    }.frame(width: 160)
                    HStack(spacing: 10) {
                        modifier(mac ? "⌘" : "Super", .logo, name: mac ? "Command" : "Super")
                        modifier(mac ? "⌃" : "Ctrl", .control, name: "Control")
                        modifier(mac ? "⌥" : "Alt", .alt, name: mac ? "Option" : "Alt")
                        modifier(mac ? "⇧" : "Shift", .shift, name: "Shift")
                    }.fixedSize(horizontal: true, vertical: false)
                }
            }
            if let error { Text(error).font(.caption).foregroundStyle(.red).fixedSize(horizontal: false, vertical: true) }
        }.accessibilityElement(children: .contain)
    }

    private func change(_ next: ShortcutChord) -> Bool {
        if next != chord, let reason = validate(next) { error = reason; return false }
        error = nil; chord = next; return true
    }

    private func modifier(_ title: String, _ mask: Windowing.Modifiers, name: String) -> some View {
        Toggle(title, isOn: Binding(get: { chord.modifiers.contains(mask) }, set: { enabled in
            var next = chord
            if enabled { next.modifiers.insert(mask) } else { next.modifiers.remove(mask) }
            _ = change(next)
        })).toggleStyle(.checkbox).accessibilityLabel(name)
    }
}

private struct ShortcutRecorder: NSViewRepresentable {
    let chord: ShortcutChord
    let onChange: (ShortcutChord) -> Bool
    let onError: (String?) -> Void
    func makeNSView(context: Context) -> ShortcutRecorderButton {
        let button = ShortcutRecorderButton(); updateNSView(button, context: context); return button
    }
    func updateNSView(_ button: ShortcutRecorderButton, context: Context) {
        button.chord = chord; button.onChange = onChange; button.onError = onError
    }
    static func dismantleNSView(_ button: ShortcutRecorderButton, coordinator: ()) { button.finishRecording() }
}

/// Intercepts local events before menu equivalents while focused. No global
/// event tap or additional sandbox permission is required to record a chord.
final class ShortcutRecorderButton: NSButton {
    var chord = ShortcutChord(.grave, .control) { didSet { if !isRecording { title = chord.label(mac: true) } } }
    var onChange: ((ShortcutChord) -> Bool)?
    var onError: ((String?) -> Void)?
    private(set) var isRecording = false
    private var pending: (code: UInt16, chord: ShortcutChord)?
    private var monitor: Any?
    private var resignObserver: NSObjectProtocol?

    init() {
        super.init(frame: .zero)
        bezelStyle = .rounded; title = chord.label(mac: true)
        target = self; action = #selector(toggleRecording)
        setAccessibilityLabel("Record shortcut")
        toolTip = "Click, then press your shortcut. Press Escape to cancel."
    }
    @available(*, unavailable) required init?(coder: NSCoder) { fatalError() }
    override var acceptsFirstResponder: Bool { true }

    @objc private func toggleRecording() {
        if isRecording { finishRecording() } else { beginRecording() }
    }
    func beginRecording() {
        guard !isRecording, let window, window.makeFirstResponder(self) else { return }
        isRecording = true; pending = nil; title = "Press shortcut…"; onError?(nil)
        NotificationCenter.default.post(name: ShortcutChordEditor.recordingDidChange, object: self, userInfo: ["recording": true])
        monitor = NSEvent.addLocalMonitorForEvents(matching: [.keyDown, .keyUp, .flagsChanged]) { [weak self] event in
            guard let self, self.isRecording, event.window === self.window else { return event }
            self.record(event); return nil
        }
        resignObserver = NotificationCenter.default.addObserver(forName: NSWindow.didResignKeyNotification,
            object: window, queue: .main) { [weak self] _ in
                MainActor.assumeIsolated { self?.finishRecording() }
            }
    }
    func record(_ event: NSEvent) {
        guard isRecording else { return }
        if event.type == .keyDown {
            guard !event.isARepeat else { return }
            if event.keyCode == 53 && event.modifierFlags.intersection([.command, .control, .option, .shift]).isEmpty {
                finishRecording(); return
            }
            guard pending == nil else { return }
            guard let value = ShortcutTranslation.chord(for: event) else {
                onError?("This key is unavailable. Choose a key from the list."); return
            }
            pending = (event.keyCode, value); title = value.label(mac: true)
        } else if event.type == .keyUp, let pending, pending.code == event.keyCode {
            // Wait for release before restoring global shortcuts, otherwise
            // the recording press can immediately activate the new binding.
            if onChange?(pending.chord) == true { finishRecording() }
            else { self.pending = nil; title = "Press shortcut…" }
        }
    }
    func finishRecording() {
        guard isRecording else { return }
        isRecording = false; pending = nil; title = chord.label(mac: true)
        onError?(nil)
        if let monitor { NSEvent.removeMonitor(monitor); self.monitor = nil }
        if let resignObserver { NotificationCenter.default.removeObserver(resignObserver); self.resignObserver = nil }
        NotificationCenter.default.post(name: ShortcutChordEditor.recordingDidChange, object: self, userInfo: ["recording": false])
    }
    override func resignFirstResponder() -> Bool { finishRecording(); return super.resignFirstResponder() }
    override func viewWillMove(toWindow newWindow: NSWindow?) {
        if newWindow !== window { finishRecording() }
        super.viewWillMove(toWindow: newWindow)
    }
}
