import Foundation
import NativePipeProtocol
import XCTest

final class InitControlWireTests: XCTestCase {
    private func append<T: FixedWidthInteger>(_ value: T, to data: inout Data) {
        var little = value.littleEndian
        withUnsafeBytes(of: &little) { data.append(contentsOf: $0) }
    }

    private func appendString(_ value: String, to data: inout Data) {
        let bytes = Data(value.utf8)
        append(UInt16(bytes.count), to: &data)
        data.append(bytes)
    }

    func testInitExecuteUsesTheSharedControlEnvelope() {
        let payload = ControlWire.encode(id: 42, call: .initExecute(.init(
            action: .repair, automatic: false, diskIdentifier: "nativepipe-root",
            root: "PARTUUID=abcd")))
        XCTAssertEqual(Data(payload.prefix(4)), Data("NPIC".utf8))
        XCTAssertEqual(payload[12], InitAction.repair.rawValue)
        XCTAssertEqual(payload[13], 0)
    }

    func testWritePathCarriesRawBytes() {
        let bytes = Data([0, 1, 2, 0xff])
        let payload = ControlWire.encode(
            id: 7, call: .writePath(path: "/etc/example", mode: 0o640, data: bytes))
        XCTAssertEqual(Data(payload.prefix(4)), Data("NPWR".utf8))
        XCTAssertTrue(payload.suffix(bytes.count).elementsEqual(bytes))
    }

    func testDesktopPreferencesUseCompactBinaryEnvelope() {
        let payload = ControlWire.encode(
            id: 11,
            call: .desktopPreferences(.init(colorScheme: .dark)))

        XCTAssertEqual(payload.count, 13)
        XCTAssertEqual(Data(payload.prefix(4)), Data("NPDP".utf8))
        XCTAssertEqual(payload.last, DesktopPreferences.ColorScheme.dark.rawValue)
    }

    func testInitInventoryResponseDecodes() {
        var payload = Data("NPIB".utf8)
        append(UInt64(9), to: &payload)
        append(UInt16(1), to: &payload)
        appendString("vda", to: &payload)
        appendString("nativepipe-root", to: &payload)
        append(UInt64(32 * 1024 * 1024), to: &payload)
        payload.append(0)
        payload.append(0)

        guard case .response(let id, .initInventory(let devices)) = ControlWire.decode(payload)
        else { return XCTFail("inventory did not decode") }
        XCTAssertEqual(id, 9)
        XCTAssertEqual(devices.count, 1)
        XCTAssertEqual(devices[0].identifier, "nativepipe-root")
        XCTAssertFalse(devices[0].readOnly)
    }

    func testRecoveryReadyHandshakeUsesExistingRuntimeEvent() {
        var payload = Data(ControlWire.runtimeReadyMagic)
        for value in [
            "initramfs-1", "6.18.46", "LightHouse Recovery", "1", "nativepipe-init",
        ] {
            appendString(value, to: &payload)
        }
        let capabilities = [
            "init.control", "init.mount", "init.execute", "fs.read", "fs.stat", "fs.write",
        ]
        append(UInt16(capabilities.count), to: &payload)
        for capability in capabilities {
            appendString(capability, to: &payload)
        }

        guard case .event(.runtimeReady(let info)) = ControlWire.decode(payload) else {
            return XCTFail("recovery ready handshake did not decode")
        }
        XCTAssertEqual(info.agentVersion, "initramfs-1")
        XCTAssertEqual(info.initSystem, "nativepipe-init")
        XCTAssertEqual(info.capabilities, capabilities)
        XCTAssertTrue(info.isRecoveryEnvironment)
    }
}
