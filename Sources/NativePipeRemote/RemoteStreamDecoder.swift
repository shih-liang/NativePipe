import Foundation
import NativePipeProtocol

/// SSH stdout contains the existing self-identifying NPIP / NPEN frames.
struct RemoteStreamDecoder {
    enum Packet {
        case event(Windowing.GuestEvent)
        case media(MediaWire.Header, Data)
        case applications(ApplicationReply)
    }
    private var buffer = Data()
    private var ready = false

    mutating func append(_ data: Data) { buffer.append(data) }

    mutating func next() throws -> Packet? {
        guard buffer.count >= 4 else { return nil }
        if buffer.prefix(4).elementsEqual(WireFormat.magic) {
            guard buffer.count >= WireFormat.headerSize else { return nil }
            let count = try WireFormat.decodeHeader(Data(buffer.prefix(WireFormat.headerSize)))
            let total = WireFormat.headerSize + count
            guard buffer.count >= total else { return nil }
            let payload = Data(buffer[WireFormat.headerSize..<total])
            buffer = Data(buffer.dropFirst(total))
            if payload.prefix(4) == ApplicationReply.magic, ready {
                return .applications(try ApplicationReply.decode(payload))
            }
            let event = try WindowWire.guestEvent(from: payload)
            if case .channelReady = event {
                guard !ready else { throw RemoteError.message("Duplicate remote session handshake.") }
                ready = true
            } else if !ready {
                throw RemoteError.message("Remote compositor did not send its session handshake.")
            }
            return .event(event)
        }
        guard buffer.prefix(4) == MediaWire.magic, ready else {
            throw RemoteError.message("Invalid remote stream. Check the compositor version and remote shell startup output.")
        }
        guard buffer.count >= MediaWire.headerSize else { return nil }
        guard let header = MediaWire.Header.parse(from: Data(buffer.prefix(MediaWire.headerSize))),
              header.payloadLength <= MediaWire.maximumPayloadSize else {
            throw RemoteError.message("Invalid remote media frame.")
        }
        let total = MediaWire.headerSize + Int(header.payloadLength)
        guard buffer.count >= total else { return nil }
        let payload = Data(buffer[MediaWire.headerSize..<total])
        buffer = Data(buffer.dropFirst(total))
        return .media(header, payload)
    }

    func finish() throws {
        guard buffer.isEmpty else { throw RemoteError.message("Truncated remote display stream.") }
    }
}
