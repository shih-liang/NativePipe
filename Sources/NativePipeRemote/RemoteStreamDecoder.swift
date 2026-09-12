import Foundation
import NativePipeProtocol

/// SSH carries short NPIP control records and credit-paced fragments of display
/// and background records. A partial image never blocks a control reply.
struct RemoteStreamDecoder {
    private(set) var packetBytes = 0
    enum Packet {
        case event(Windowing.GuestEvent)
        case media(MediaWire.Header, Data)
        case applications(ApplicationReply)
        case acknowledge(Int)
        case credit(Int)
    }
    private var buffer = Data()
    private var ready = false
    private var fragments = RemoteWire.Reassembler()
    private var completed: Data?

    mutating func append(_ data: Data) { buffer.append(data) }

    mutating func next() throws -> Packet? {
        if let record = completed {
            completed = nil
            return try decodeRecord(record)
        }
        guard buffer.count >= 4 else { return nil }
        if buffer.prefix(4).elementsEqual(WireFormat.magic) {
            guard buffer.count >= WireFormat.headerSize else { return nil }
            let count = try WireFormat.decodeHeader(Data(buffer.prefix(WireFormat.headerSize)))
            let total = WireFormat.headerSize + count
            guard buffer.count >= total else { return nil }
            let payload = Data(buffer[WireFormat.headerSize..<total])
            buffer = Data(buffer.dropFirst(total))
            if payload.prefix(4) == RemoteWire.fragmentMagic {
                completed = try fragments.receive(payload, maximumSize: MediaWire.maximumPayloadSize + MediaWire.headerSize)
                return .acknowledge(payload.count - 16)
            }
            if payload.prefix(4) == RemoteWire.acknowledgementMagic {
                guard payload.count == 8 else { throw RemoteError.message("Invalid remote transport credit.") }
                return .credit(Int(RemoteWire.number(payload, at: 4)))
            }
            return try decodeControl(payload)
        }
        throw RemoteError.message("Invalid remote stream. Update the remote compositor to match this client.")
    }

    private mutating func decodeControl(_ payload: Data) throws -> Packet {
        packetBytes = payload.count
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

    private mutating func decodeRecord(_ record: Data) throws -> Packet {
        if record.prefix(4).elementsEqual(WireFormat.magic) {
            let size = try WireFormat.decodeHeader(Data(record.prefix(WireFormat.headerSize)))
            guard record.count == WireFormat.headerSize + size else { throw RemoteError.message("Invalid remote record size.") }
            return try decodeControl(Data(record.dropFirst(WireFormat.headerSize)))
        }
        guard ready, let header = MediaWire.Header.parse(from: Data(record.prefix(MediaWire.headerSize))),
              header.payloadLength <= MediaWire.maximumPayloadSize,
              record.count == MediaWire.headerSize + Int(header.payloadLength) else {
            throw RemoteError.message("Invalid remote media frame.")
        }
        return .media(header, Data(record.dropFirst(MediaWire.headerSize)))
    }

    func finish() throws {
        guard buffer.isEmpty, fragments.isEmpty, completed == nil else { throw RemoteError.message("Truncated remote display stream.") }
    }
}
