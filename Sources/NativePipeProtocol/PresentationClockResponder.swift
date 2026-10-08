import QuartzCore

/// Calibration belongs to the connection's reader: UI queueing is not part of
/// transport RTT. The caller validates its handshake/generation before sending
/// this reply, then delivers the original event to the UI to adopt the session.
public enum PresentationClockResponder {
    public static func sample(for event: Windowing.GuestEvent) -> Windowing.HostCommand? {
        guard case .presentationClockRequested(let token, let session, let epoch) = event else { return nil }
        let nanoseconds = UInt64((CACurrentMediaTime() * 1_000_000_000).rounded())
        return .presentationClockSample(token: token, sessionID: session,
            clockEpoch: epoch, hostTimeNanoseconds: nanoseconds)
    }
}
