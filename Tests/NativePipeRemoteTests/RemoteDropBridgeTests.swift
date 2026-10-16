import Darwin
import Foundation
import XCTest
@testable import NativePipeProtocol
@testable import NativePipeRemote

final class RemoteDropBridgeTests: XCTestCase {
    private func ready() throws -> Data {
        var payload = Data(WindowWire.lifecycleMagic + [1, 1, 0, 0])
        var pid: UInt32 = 123
        var version = WindowWire.windowProtocolVersion.littleEndian
        withUnsafeBytes(of: &pid) { payload.append(contentsOf: $0) }
        withUnsafeBytes(of: &version) { payload.append(contentsOf: $0) }
        return try WireFormat.frame(payload: payload)
    }

    func testDecoderDeliversFileSessionRecordsInlineAndFragmented() throws {
        var early = RemoteStreamDecoder()
        early.append(try WireFormat.frame(payload: RemoteWire.fileSessionMagic + Data([1])))
        XCTAssertThrowsError(try early.next(), "nothing before the session handshake")

        var decoder = RemoteStreamDecoder()
        decoder.append(try ready())
        _ = try decoder.next()
        decoder.append(try WireFormat.frame(payload: RemoteWire.fileSessionMagic + Data("abc".utf8)))
        guard case .fileSession(let inline) = try decoder.next() else { return XCTFail("Missing session bytes") }
        XCTAssertEqual(inline, Data("abc".utf8))
        let record = try WireFormat.frame(payload: RemoteWire.fileSessionMagic + Data(repeating: 7, count: 40_000))
        for offset in stride(from: 0, to: record.count, by: RemoteWire.fragmentSize) {
            decoder.append(RemoteWire.fragment(record, offset: offset, lane: 2))
            guard case .acknowledge = try decoder.next() else { return XCTFail("Missing credit") }
        }
        guard case .fileSession(let bulk) = try decoder.next() else { return XCTFail("Missing bulk session bytes") }
        XCTAssertEqual(bulk, Data(repeating: 7, count: 40_000))
    }

    func testLaunchScriptAnnouncesDrops() {
        let command = SSHCommand(destination: "host", application: ["true"])
        XCTAssertTrue(command.makeRemoteScript(uploadCompositor: false).contains("export NATIVEPIPE_HOST_DROPS=1\n"))
    }

    /// The whole host side: bridge, writer and DropFileServer, against a guest
    /// session reached only through NPFS records, as over SSH.
    func testDroppedFileIsReadOverTheStreamAndNothingIsSentFirst() async throws {
        var output: [Int32] = [-1, -1]
        XCTAssertEqual(pipe(&output), 0)
        let writer = WindowCommandWriter(remote: true) { handle, data in try handle.write(contentsOf: data) }
        writer.install(FileHandle(fileDescriptor: output[1], closeOnDealloc: true))
        let directory = FileManager.default.temporaryDirectory.appendingPathComponent("remote-drops-" + UUID().uuidString)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: directory) }
        let bridge = RemoteDropBridge(stagingDirectory: directory.appendingPathComponent("staging"))
        bridge.connect(writer: writer)
        defer { bridge.disconnect(); writer.disconnect(); Darwin.close(output[0]) }

        // Nothing goes to a remote that has not spoken: an older compositor
        // would reject the record.
        try await Task.sleep(nanoseconds: 100_000_000)
        var probe = pollfd(fd: output[0], events: Int16(POLLIN), revents: 0)
        XCTAssertEqual(poll(&probe, 1, 0), 0)

        // The remote compositor: its session socket and the bytes it exchanges.
        var remote: [Int32] = [-1, -1]
        XCTAssertEqual(socketpair(AF_UNIX, SOCK_STREAM, 0, &remote), 0)
        let guest = FileSession(connection: try SocketConnection(owning: remote[0]), role: .guest)
        let bridgeEnd = remote[1]
        // Remote -> Mac: what the compositor's pump would send as NPFS records.
        Thread.detachNewThread {
            var buffer = [UInt8](repeating: 0, count: 60_000)
            while true {
                let count = buffer.withUnsafeMutableBytes { Darwin.read(bridgeEnd, $0.baseAddress, $0.count) }
                if count <= 0 { return }
                bridge.receive(Data(buffer[0..<count]))
            }
        }
        // Mac -> remote: reassemble bulk-lane records, credit them like the remote.
        let reader = output[0]
        Thread.detachNewThread {
            var pending = Data(), fragments = RemoteWire.Reassembler()
            var buffer = [UInt8](repeating: 0, count: 65_536)
            while true {
                let count = buffer.withUnsafeMutableBytes { Darwin.read(reader, $0.baseAddress, $0.count) }
                if count <= 0 { return }
                pending.append(contentsOf: buffer[0..<count])
                while pending.count >= WireFormat.headerSize,
                      let size = try? WireFormat.decodeHeader(Data(pending.prefix(WireFormat.headerSize))),
                      pending.count >= WireFormat.headerSize + size {
                    let payload = Data(pending[WireFormat.headerSize..<WireFormat.headerSize + size])
                    pending = Data(pending.dropFirst(WireFormat.headerSize + size))
                    guard payload.prefix(4) == RemoteWire.fragmentMagic,
                          let record = try? fragments.receive(payload, maximumSize: 1 << 20) else {
                        if payload.prefix(4) == RemoteWire.fragmentMagic { writer.acknowledgeRemoteBytes(payload.count - 16, received: false) }
                        continue
                    }
                    writer.acknowledgeRemoteBytes(payload.count - 16, received: false)
                    let inner = Data(record.dropFirst(WireFormat.headerSize))
                    guard inner.prefix(4) == RemoteWire.fileSessionMagic else { continue }
                    let bytes = Data(inner.dropFirst(4))
                    bytes.withUnsafeBytes { _ = Darwin.write(bridgeEnd, $0.baseAddress, $0.count) }
                }
            }
        }
        guest.start()
        defer { guest.close() }
        try await guest.notify(DropFileServer.Notification.mounted, body: Data("/run/user/1000/nativepipe-drops".utf8))
        for _ in 0..<500 where !bridge.isReady { try await Task.sleep(nanoseconds: 2_000_000) }
        XCTAssertTrue(bridge.isReady)

        let contents = Data((0..<300_000).map { UInt8(truncatingIfNeeded: $0 &* 13) })
        let file = directory.appendingPathComponent("photo.raw")
        try contents.write(to: file)
        let url = try XCTUnwrap(bridge.share(file))
        XCTAssertTrue(url.path.hasPrefix("/run/user/1000/nativepipe-drops/"))
        let token = url.pathComponents[url.pathComponents.count - 2]

        let open = try await guest.request(DropFileServer.Opcode.open,
                                           body: FileSession.encode(0) + Data(token.utf8) + Data("photo.raw".utf8), end: true)
        let response = try await open.receive()
        let opened = try XCTUnwrap(response)
        let handle: UInt64 = opened.body.withUnsafeBytes { UInt64(littleEndian: $0.loadUnaligned(as: UInt64.self)) }
        var received = Data()
        while received.count < contents.count {
            var body = DropFileServer.Attributes.encode64(handle) + DropFileServer.Attributes.encode64(UInt64(received.count))
            body.append(FileSession.encode(UInt32(131_072)))
            let read = try await guest.request(DropFileServer.Opcode.read, body: body, end: true)
            guard case .response(0)? = try await read.receive()?.kind else { return XCTFail("Read failed") }
            var chunk = Data()
            while let message = try await read.receive() { chunk.append(message.body) }
            if chunk.isEmpty { break }
            received.append(chunk)
        }
        XCTAssertEqual(received, contents)
    }
}
