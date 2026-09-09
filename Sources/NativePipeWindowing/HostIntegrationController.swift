import AppKit
import Carbon
import NativePipeProtocol

/// Extracted from VMHost: the authoritative macOS appearance/input observers
/// and resolution rules. Backends only deliver the resolved values.
@MainActor
public final class HostIntegrationController {
    public var applyWindows: ((WindowIntegrationPreferences) -> Void)?
    public var applyDesktop: ((DesktopPreferences) -> Void)?
    private var appearance: DesktopPreferences.ColorScheme?
    private var windows = WindowIntegrationPreferences(keyboardLayout: "")
    private var appearanceObservation: NSKeyValueObservation?
    private var inputObservation: NSObjectProtocol?

    public init() {
        appearanceObservation = NSApp.observe(\.effectiveAppearance, options: [.new]) { [weak self] _, _ in
            DispatchQueue.main.async { self?.syncDesktop() }
        }
        inputObservation = DistributedNotificationCenter.default().addObserver(
            forName: Notification.Name(kTISNotifySelectedKeyboardInputSourceChanged as String),
            object: nil, queue: .main
        ) { [weak self] _ in
            MainActor.assumeIsolated {
                guard self?.windows.keyboardLayout.isEmpty == true else { return }
                self?.syncWindows()
            }
        }
    }

    deinit {
        if let inputObservation { DistributedNotificationCenter.default().removeObserver(inputObservation) }
    }

    public func update(windows: WindowIntegrationPreferences, appearance: DesktopPreferences.ColorScheme?) {
        self.windows = windows
        self.appearance = appearance
        sync()
    }

    public func sync() { syncWindows(); syncDesktop() }

    private func syncWindows() {
        var value = windows
        if value.keyboardLayout.isEmpty { value.keyboardLayout = HostKeyboardLayout.current() }
        applyWindows?(value)
    }

    private func syncDesktop() {
        let scheme = appearance ?? (NSApp.effectiveAppearance.bestMatch(from: [.darkAqua, .aqua]) == .darkAqua ? .dark : .light)
        applyDesktop?(.init(colorScheme: scheme))
    }
}
