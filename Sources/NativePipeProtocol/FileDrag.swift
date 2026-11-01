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

/// Data conventions shared with the compositor. Every field is optional on the
/// wire, so an older peer that sends none is read as offering files.
public extension FileDragMessage {
    static let fileList = "text/uri-list"

    /// `offered` / `enter`: the MIME types a drag carries, one per line.
    static func typeList(_ types: [String]) -> Data { Data(types.joined(separator: "\n").utf8) }
    static func types(in data: Data?) -> [String] {
        guard let data, !data.isEmpty, let text = String(data: data, encoding: .utf8) else { return [fileList] }
        let types = text.split(separator: "\n").map(String.init).filter { !$0.isEmpty && $0.utf8.count < 256 }
        return types.isEmpty ? [fileList] : types
    }

    /// `requestData` / `readSource` / `exportBegan`: one MIME type.
    static func type(in data: Data?) -> String {
        guard let data, !data.isEmpty, data.count < 256, !data.contains(0),
              let type = String(data: data, encoding: .utf8) else { return fileList }
        return type
    }

    /// `payload` / `sourceData`: "mime\0bytes". Untyped bytes are a file list
    /// (a URI list never contains NUL).
    static func tagged(_ type: String, _ bytes: Data) -> Data {
        var data = Data(type.utf8); data.append(0); data.append(bytes); return data
    }
    static func untag(_ data: Data) -> (type: String, bytes: Data) {
        guard let nul = data.prefix(256).firstIndex(of: 0),
              let type = String(data: data[data.startIndex..<nul], encoding: .utf8), !type.isEmpty else {
            return (fileList, data)
        }
        return (type, Data(data[data.index(after: nul)...]))
    }
}
