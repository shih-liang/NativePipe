import Foundation

/// File DnD control only; file contents travel over user-vsock or SFTP.
public struct FileDragMessage: Sendable, Equatable {
    public enum Action: UInt32, Sendable {
        case enter = 1, motion, leave, drop, payload, readSource, exportBegan, exportEnded, exportDropped
        case offered = 101, sourceData, accepted, finished, requestData
    }
    public var action: Action
    public var token: UInt32
    public var window: UInt32
    public var x: Double
    public var y: Double
    public var data: Data?
    public init(_ action: Action, token: UInt32, window: UInt32 = 0, x: Double = 0, y: Double = 0, data: Data? = nil) {
        self.action = action; self.token = token; self.window = window
        self.x = x; self.y = y; self.data = data
    }
}
