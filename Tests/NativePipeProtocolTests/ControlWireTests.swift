import Foundation
import Testing
@testable import NativePipeProtocol

@Test func execWireCarriesTerminalModeBeforeLaunchSpec() throws {
    let interactive = ControlWire.encode(
        id: 7,
        call: .exec(
            spec: .init(executable: "/bin/cat"),
            cols: 80, rows: 24, terminal: true))
    let redirected = ControlWire.encode(
        id: 7,
        call: .exec(
            spec: .init(executable: "/bin/cat"),
            cols: 80, rows: 24, terminal: false))

    #expect(interactive.count == redirected.count)
    #expect(interactive.prefix(20) == redirected.prefix(20))
    #expect(interactive[20..<24] == Data([1, 0, 0, 0]))
    #expect(redirected[20..<24] == Data([0, 0, 0, 0]))
    #expect(interactive.dropFirst(24) == redirected.dropFirst(24))
}
