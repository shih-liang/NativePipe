import Foundation

/// Binary frames on the guestd control channel (NPIP payload).
///
/// Same outer NPIP framing as mouse (`WindowWire`): 4-byte magic, then
/// little-endian fields. Magics are the only discriminator — JSON is not
/// used on this channel.
///
/// Host → guest:
///     NPHI  hello         id(u64) ver_len(u16) ver
///     NPPG  ping          id(u64)
///     NPVQ  getVersion    id(u64)
///     NPEU  refresh env   id(u64)
///     NPSY  sync resources id(u64) guestd_ver(str) env_revision(u64) profile(str)
///     NPDP  desktop prefs id(u64) color_scheme(u8)
///     NPSF  shared folders id(u64) mounted(u8: 0 or 1)
///     NPRZ  resize        id(u64) cols(u32) rows(u32)
///     NPLN  launch        id(u64) LaunchSpec
///     NPRU  run           id(u64) LaunchSpec
///     NPXC  exec          id(u64) cols(u32) rows(u32) terminal(u32) LaunchSpec
///     NPSH  shutdown      id(u64)
///     NPUS  setUser       id(u64) user_len(u16) user flags(u8) [old_len(u16) old]
///     NPWP  setPassword   id(u64) user_len(u16) user pass_len(u16) pass
///     NPIH  init inventory id(u64)
///     NPIM  init mount    id(u64) identifier(str) partition(u16) writable(u8) reserved(u8)
///     NPIC  init execute  id(u64) action(u8) automatic(u8) reserved(u16) five strings
///
/// Guest → host:
///     NPOK  ok            id(u64)
///     NPVV  version       id(u64) ver_len(u16) ver
///     NPIF  hello info    id(u64) GuestInfo
///     NPLP  launched      id(u64) pid(i32)
///     NPRX  ran           id(u64) status(i32) out_len(u32) out err_len(u32) err
///     NPXS  exec session  id(u64) pid(i32) port(u32)
///     NPIB  block devices id(u64) count(u16) entries
///     NPER  error         id(u64) code(u32) msg_len(u16) msg
///
/// Guest → host events (no request id):
///     NPRT  runtimeReady  GuestInfo (also recovery init's outbound handshake)
///     NPEX  processExited pid(i32) status(i32)
///     NPLG  log           level_len(u16) level msg_len(u16) msg
///
/// LaunchSpec:
///     exe_len(u16) exe cwd_len(u16) cwd
///     arg_count(u16) [len(u16) bytes]*
///     env_count(u16) [key_len(u16) key val_len(u16) val]*
///     stdin_len(u32) stdin
///
/// GuestInfo:
///     five length-prefixed strings (agentVersion, kernelRelease, distroName,
///     distroVersion, initSystem), then cap_count(u16) and that many strings,
///     optionally environmentProfile and environmentRevision strings.
///     New guests append os-release ID, ID_LIKE and architecture strings.
///
/// NPLS `flags`: bit0 = more remains, bit1 = last name truncated (split inside
/// a name). Entry: dtype(u8) eflags(u8) name_len(u16) name.
public enum ControlWire {
    // Host → guest
    public static let helloMagic: [UInt8] = Array("NPHI".utf8)
    public static let pingMagic: [UInt8] = Array("NPPG".utf8)
    public static let getVersionMagic: [UInt8] = Array("NPVQ".utf8)
    public static let refreshEnvironmentMagic: [UInt8] = Array("NPEU".utf8)
    public static let reconcileResourcesMagic: [UInt8] = Array("NPSY".utf8)
    public static let desktopPreferencesMagic: [UInt8] = Array("NPDP".utf8)
    public static let sharedFoldersMagic: [UInt8] = Array("NPSF".utf8)
    public static let resizeMagic: [UInt8] = Array("NPRZ".utf8)
    public static let launchMagic: [UInt8] = Array("NPLN".utf8)
    public static let runMagic: [UInt8] = Array("NPRU".utf8)
    public static let execMagic: [UInt8] = Array("NPXC".utf8)
    public static let shutdownMagic: [UInt8] = Array("NPSH".utf8)
    public static let setUserMagic: [UInt8] = Array("NPUS".utf8)
    public static let setPasswordMagic: [UInt8] = Array("NPWP".utf8)
    public static let initInventoryMagic: [UInt8] = Array("NPIH".utf8)
    public static let initMountMagic: [UInt8] = Array("NPIM".utf8)
    public static let initExecuteMagic: [UInt8] = Array("NPIC".utf8)

    // Guest → host
    public static let okMagic: [UInt8] = Array("NPOK".utf8)
    public static let versionMagic: [UInt8] = Array("NPVV".utf8)
    public static let infoMagic: [UInt8] = Array("NPIF".utf8)
    public static let launchedMagic: [UInt8] = Array("NPLP".utf8)
    public static let ranMagic: [UInt8] = Array("NPRX".utf8)
    public static let execSessionMagic: [UInt8] = Array("NPXS".utf8)
    public static let initInventoryResultMagic: [UInt8] = Array("NPIB".utf8)
    public static let errorMagic: [UInt8] = Array("NPER".utf8)

    // Events
    public static let runtimeReadyMagic: [UInt8] = Array("NPRT".utf8)
    public static let processExitedMagic: [UInt8] = Array("NPEX".utf8)
    public static let logMagic: [UInt8] = Array("NPLG".utf8)

    public static let setUserHasOld: UInt8 = 1

    public enum Decoded: Sendable {
        case response(id: UInt64, result: Response.Result)
        case event(Event)
    }

    public static func magic(of payload: Data) -> [UInt8]? {
        guard payload.count >= 4 else { return nil }
        return Array(payload.prefix(4))
    }

    public static func isControlFrame(_ payload: Data) -> Bool {
        guard let magic = magic(of: payload) else { return false }
        return knownMagics.contains(magic)
    }

    private static let knownMagics: Set<[UInt8]> = [
        helloMagic, pingMagic, getVersionMagic, refreshEnvironmentMagic, reconcileResourcesMagic,
        desktopPreferencesMagic, sharedFoldersMagic,
        resizeMagic, launchMagic, runMagic,
        execMagic, shutdownMagic,
        setUserMagic, setPasswordMagic, initInventoryMagic, initMountMagic, initExecuteMagic,
        okMagic, versionMagic, infoMagic, launchedMagic, ranMagic,
        execSessionMagic, errorMagic, runtimeReadyMagic,
        initInventoryResultMagic, processExitedMagic, logMagic,
    ]

    // MARK: - Encode requests

    public static func encode(id: UInt64, call: Request.Call) -> Data {
        switch call {
        case .hello(let hostVersion):
            var payload = Data(helloMagic)
            append(id, to: &payload)
            appendString(hostVersion, to: &payload)
            return payload
        case .ping:
            var payload = Data(pingMagic)
            append(id, to: &payload)
            return payload
        case .getVersion:
            var payload = Data(getVersionMagic)
            append(id, to: &payload)
            return payload
        case .refreshEnvironment:
            var payload = Data(refreshEnvironmentMagic)
            append(id, to: &payload)
            return payload
        case .reconcileResources(let desired):
            var payload = Data(reconcileResourcesMagic)
            append(id, to: &payload)
            appendString(desired.guestdVersion, to: &payload)
            append(desired.environmentRevision, to: &payload)
            appendString(desired.environmentProfile, to: &payload)
            return payload
        case .desktopPreferences(let preferences):
            var payload = Data(desktopPreferencesMagic)
            append(id, to: &payload)
            payload.append(preferences.colorScheme.rawValue)
            return payload
        case .setSharedFoldersMounted(let mounted):
            var payload = Data(sharedFoldersMagic)
            append(id, to: &payload)
            payload.append(mounted ? 1 : 0)
            return payload
        case .resizeConsole(let cols, let rows):
            var payload = Data(resizeMagic)
            append(id, to: &payload)
            append(UInt32(clamping: cols), to: &payload)
            append(UInt32(clamping: rows), to: &payload)
            return payload
        case .launch(let spec):
            var payload = Data(launchMagic)
            append(id, to: &payload)
            appendLaunchSpec(spec, to: &payload)
            return payload
        case .run(let spec):
            var payload = Data(runMagic)
            append(id, to: &payload)
            appendLaunchSpec(spec, to: &payload)
            return payload
        case .exec(let spec, let cols, let rows, let terminal):
            var payload = Data(execMagic)
            append(id, to: &payload)
            append(UInt32(clamping: cols), to: &payload)
            append(UInt32(clamping: rows), to: &payload)
            append(terminal ? UInt32(1) : UInt32(0), to: &payload)
            appendLaunchSpec(spec, to: &payload)
            return payload
        case .initInventory:
            var payload = Data(initInventoryMagic)
            append(id, to: &payload)
            return payload
        case .initMount(let identifier, let partition, let writable):
            var payload = Data(initMountMagic)
            append(id, to: &payload)
            appendString(identifier, to: &payload)
            append(partition, to: &payload)
            payload.append(writable ? 1 : 0)
            payload.append(0)
            return payload
        case .initExecute(let plan):
            var payload = Data(initExecuteMagic)
            append(id, to: &payload)
            payload.append(plan.action.rawValue)
            payload.append(plan.automatic ? 1 : 0)
            payload.append(contentsOf: [0, 0])
            appendString(plan.diskIdentifier, to: &payload)
            appendString(plan.root, to: &payload)
            appendString(plan.payloadTag, to: &payload)
            appendString(plan.adapterPath, to: &payload)
            appendString(plan.sourcePath, to: &payload)
            return payload
        case .shutdown:
            var payload = Data(shutdownMagic)
            append(id, to: &payload)
            return payload
        case .setUser(let username, let oldUsername):
            var payload = Data(setUserMagic)
            append(id, to: &payload)
            appendString(username, to: &payload)
            if let oldUsername {
                payload.append(setUserHasOld)
                appendString(oldUsername, to: &payload)
            } else {
                payload.append(0)
            }
            return payload
        case .setPassword(let username, let password):
            var payload = Data(setPasswordMagic)
            append(id, to: &payload)
            appendString(username, to: &payload)
            appendString(password, to: &payload)
            return payload
        }
    }


    // MARK: - Decode guest → host

    public static func decode(_ payload: Data) -> Decoded? {
        guard let magic = magic(of: payload) else { return nil }
        var offset = 4

        if magic == runtimeReadyMagic {
            guard let info = takeGuestInfo(&offset, from: payload) else { return nil }
            return .event(.runtimeReady(info: info))
        }
        if magic == processExitedMagic {
            guard let pid: Int32 = take(&offset, from: payload),
                  let status: Int32 = take(&offset, from: payload) else { return nil }
            return .event(.processExited(pid: pid, status: status))
        }
        if magic == logMagic {
            guard let level = takeString(&offset, from: payload),
                  let message = takeString(&offset, from: payload) else { return nil }
            return .event(.log(level: level, message: message))
        }

        guard let id: UInt64 = take(&offset, from: payload) else { return nil }

        if magic == okMagic {
            return .response(id: id, result: .ok)
        }
        if magic == versionMagic {
            guard let ver = takeString(&offset, from: payload) else { return nil }
            return .response(id: id, result: .version(ver))
        }
        if magic == infoMagic {
            guard let info = takeGuestInfo(&offset, from: payload) else { return nil }
            return .response(id: id, result: .hello(info: info))
        }
        if magic == launchedMagic {
            guard let pid: Int32 = take(&offset, from: payload) else { return nil }
            return .response(id: id, result: .launched(pid: pid))
        }
        if magic == ranMagic {
            guard let status: Int32 = take(&offset, from: payload),
                  let outLen: UInt32 = take(&offset, from: payload),
                  offset + Int(outLen) <= payload.count else { return nil }
            let outData = payload.subdata(in: offset..<(offset + Int(outLen)))
            offset += Int(outLen)
            guard let errLen: UInt32 = take(&offset, from: payload),
                  offset + Int(errLen) <= payload.count else { return nil }
            let errData = payload.subdata(in: offset..<(offset + Int(errLen)))
            return .response(
                id: id,
                result: .ran(
                    status: status,
                    stdout: String(decoding: outData, as: UTF8.self),
                    stderr: String(decoding: errData, as: UTF8.self)))
        }
        if magic == execSessionMagic {
            guard let pid: Int32 = take(&offset, from: payload),
                  let port: UInt32 = take(&offset, from: payload) else { return nil }
            return .response(id: id, result: .execSession(pid: pid, port: port))
        }
        if magic == initInventoryResultMagic {
            guard let count: UInt16 = take(&offset, from: payload) else { return nil }
            var devices: [InitBlockDevice] = []
            devices.reserveCapacity(Int(count))
            for _ in 0..<count {
                guard let name = takeString(&offset, from: payload),
                      let identifier = takeString(&offset, from: payload),
                      let size: UInt64 = take(&offset, from: payload),
                      offset + 2 <= payload.count else { return nil }
                let readOnly = payload[offset] != 0
                let isPartition = payload[offset + 1] != 0
                offset += 2
                devices.append(.init(
                    name: name, identifier: identifier, sizeBytes: size,
                    readOnly: readOnly, isPartition: isPartition))
            }
            guard offset == payload.count else { return nil }
            return .response(id: id, result: .initInventory(devices))
        }
        if magic == errorMagic {
            guard let code: UInt32 = take(&offset, from: payload),
                  let message = takeString(&offset, from: payload) else { return nil }
            return .response(
                id: id,
                result: .failure(code: Int32(bitPattern: code), message: message))
        }
        return nil
    }

    // MARK: - Helpers

    private static func appendLaunchSpec(_ spec: LaunchSpec, to data: inout Data) {
        appendString(spec.executable, to: &data)
        appendString(spec.workingDirectory ?? "", to: &data)
        append(UInt16(clamping: spec.arguments.count), to: &data)
        for arg in spec.arguments {
            appendString(arg, to: &data)
        }
        let env = spec.environment.sorted { $0.key < $1.key }
        append(UInt16(clamping: env.count), to: &data)
        for (key, value) in env {
            appendString(key, to: &data)
            appendString(value, to: &data)
        }
        let stdin = Data((spec.stdin ?? "").utf8)
        append(UInt32(clamping: stdin.count), to: &data)
        data.append(stdin)
        // Optional tail keeps nil-user calls byte-for-byte compatible with
        // guestd 0.2.5. Only desktop launches require the 0.2.6 extension.
        if let username = spec.username {
            appendString(username, to: &data)
        }
    }

    private static func takeGuestInfo(_ offset: inout Int, from data: Data) -> GuestInfo? {
        guard let agentVersion = takeString(&offset, from: data),
              let kernelRelease = takeString(&offset, from: data),
              let distroName = takeString(&offset, from: data),
              let distroVersion = takeString(&offset, from: data),
              let initSystem = takeString(&offset, from: data),
              let capCount: UInt16 = take(&offset, from: data) else { return nil }
        var capabilities: [String] = []
        capabilities.reserveCapacity(Int(capCount))
        for _ in 0..<capCount {
            guard let cap = takeString(&offset, from: data) else { return nil }
            capabilities.append(cap)
        }
        var environmentProfile: String?
        var environmentRevision: UInt64?
        var environmentID: String?
        var environmentIDLike: [String] = []
        var architecture: String?
        if offset < data.count {
            guard let profile = takeString(&offset, from: data) else { return nil }
            environmentProfile = profile.isEmpty ? nil : profile
        }
        if offset < data.count {
            guard let revision = takeString(&offset, from: data) else { return nil }
            environmentRevision = UInt64(revision)
        }
        if offset < data.count {
            guard let id = takeString(&offset, from: data) else { return nil }
            environmentID = id.isEmpty ? nil : id
        }
        if offset < data.count {
            guard let idLike = takeString(&offset, from: data) else { return nil }
            environmentIDLike = idLike.split(whereSeparator: { $0 == " " || $0 == "\t" })
                .map(String.init)
        }
        if offset < data.count {
            guard let arch = takeString(&offset, from: data) else { return nil }
            architecture = arch.isEmpty ? nil : arch
        }
        return GuestInfo(
            agentVersion: agentVersion,
            kernelRelease: kernelRelease,
            distroName: distroName,
            distroVersion: distroVersion,
            initSystem: initSystem,
            capabilities: capabilities,
            environmentProfile: environmentProfile,
            environmentRevision: environmentRevision,
            environmentID: environmentID,
            environmentIDLike: environmentIDLike,
            architecture: architecture)
    }


    private static func take<T: FixedWidthInteger>(_ offset: inout Int, from data: Data) -> T? {
        let size = MemoryLayout<T>.size
        guard offset + size <= data.count else { return nil }
        let slice = data[offset..<(offset + size)]
        offset += size
        var value: T = 0
        _ = withUnsafeMutableBytes(of: &value) { dest in
            slice.copyBytes(to: dest, count: size)
        }
        return T(littleEndian: value)
    }

    private static func takeString(_ offset: inout Int, from data: Data) -> String? {
        guard let count: UInt16 = take(&offset, from: data) else { return nil }
        let n = Int(count)
        guard offset + n <= data.count else { return nil }
        let slice = data[offset..<(offset + n)]
        offset += n
        return String(data: slice, encoding: .utf8) ?? String(decoding: slice, as: UTF8.self)
    }

    private static func appendString(_ value: String, to data: inout Data) {
        let bytes = Data(value.utf8)
        append(UInt16(clamping: bytes.count), to: &data)
        data.append(bytes)
    }

    private static func append<T: FixedWidthInteger>(_ value: T, to data: inout Data) {
        var little = value.littleEndian
        Swift.withUnsafeBytes(of: &little) { data.append(contentsOf: $0) }
    }
}
