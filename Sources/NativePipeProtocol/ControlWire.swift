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
///     NPRZ  resize        id(u64) cols(u32) rows(u32)
///     NPLN  launch        id(u64) LaunchSpec
///     NPRU  run           id(u64) LaunchSpec
///     NPRE  read path     id(u64) path_len(u16) path
///     NPCT  list continue id(u64)
///     NPCL  list cancel   id(u64)
///     NPMS  stat path     id(u64) path_len(u16) path
///     NPSH  shutdown      id(u64)
///     NPUS  setUser       id(u64) user_len(u16) user flags(u8) [old_len(u16) old]
///     NPWP  setPassword   id(u64) user_len(u16) user pass_len(u16) pass
///
/// Guest → host:
///     NPOK  ok            id(u64)
///     NPIF  hello info    id(u64) GuestInfo
///     NPLP  launched      id(u64) pid(i32)
///     NPRX  ran           id(u64) status(i32) out_len(u32) out err_len(u32) err
///     NPFL  file bytes    id(u64) path_len(u16) path size(u64) bytes
///     NPLS  dir listing   id(u64) path_len(u16) path flags(u32) count(u32) entries
///     NPFS  path stat     id(u64) path_len(u16) path mode(u32) uid(u32) gid(u32) size(u64) mtime(i64)
///     NPER  error         id(u64) code(u32) msg_len(u16) msg
///
/// Guest → host events (no request id):
///     NPRT  runtimeReady  GuestInfo
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
///     distroVersion, initSystem), then cap_count(u16) and that many strings.
///
/// NPLS `flags`: bit0 = more remains, bit1 = last name truncated (split inside
/// a name). Entry: dtype(u8) eflags(u8) name_len(u16) name.
public enum ControlWire {
    // Host → guest
    public static let helloMagic: [UInt8] = Array("NPHI".utf8)
    public static let pingMagic: [UInt8] = Array("NPPG".utf8)
    public static let resizeMagic: [UInt8] = Array("NPRZ".utf8)
    public static let launchMagic: [UInt8] = Array("NPLN".utf8)
    public static let runMagic: [UInt8] = Array("NPRU".utf8)
    public static let readMagic: [UInt8] = Array("NPRE".utf8)
    public static let continueMagic: [UInt8] = Array("NPCT".utf8)
    public static let cancelMagic: [UInt8] = Array("NPCL".utf8)
    public static let statMagic: [UInt8] = Array("NPMS".utf8)
    public static let shutdownMagic: [UInt8] = Array("NPSH".utf8)
    public static let setUserMagic: [UInt8] = Array("NPUS".utf8)
    public static let setPasswordMagic: [UInt8] = Array("NPWP".utf8)

    // Guest → host
    public static let okMagic: [UInt8] = Array("NPOK".utf8)
    public static let infoMagic: [UInt8] = Array("NPIF".utf8)
    public static let launchedMagic: [UInt8] = Array("NPLP".utf8)
    public static let ranMagic: [UInt8] = Array("NPRX".utf8)
    public static let fileMagic: [UInt8] = Array("NPFL".utf8)
    public static let listMagic: [UInt8] = Array("NPLS".utf8)
    public static let pathStatMagic: [UInt8] = Array("NPFS".utf8)
    public static let errorMagic: [UInt8] = Array("NPER".utf8)

    // Events
    public static let runtimeReadyMagic: [UInt8] = Array("NPRT".utf8)
    public static let processExitedMagic: [UInt8] = Array("NPEX".utf8)
    public static let logMagic: [UInt8] = Array("NPLG".utf8)

    public static let listNameBudget = 64 * 1024
    public static let listHasMore: UInt32 = 1
    public static let listNameTruncated: UInt32 = 2
    public static let entryContinuesName: UInt8 = 1
    public static let entryNameIncomplete: UInt8 = 2
    public static let setUserHasOld: UInt8 = 1

    public enum Decoded: Sendable {
        case response(id: UInt64, result: Response.Result)
        case event(Event)
        case listing(id: UInt64, contents: PathContents, hasMore: Bool, nameTruncated: Bool)
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
        helloMagic, pingMagic, resizeMagic, launchMagic, runMagic, readMagic,
        continueMagic, cancelMagic, statMagic, shutdownMagic, setUserMagic,
        setPasswordMagic, okMagic, infoMagic, launchedMagic, ranMagic, fileMagic,
        listMagic, pathStatMagic, errorMagic, runtimeReadyMagic, processExitedMagic,
        logMagic,
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
        case .readPath(let path):
            return encodeRead(id: id, path: path)
        case .statPath(let path):
            var payload = Data(statMagic)
            append(id, to: &payload)
            appendString(path, to: &payload)
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

    public static func encodeRead(id: UInt64, path: String) -> Data {
        var payload = Data(readMagic)
        append(id, to: &payload)
        appendString(path, to: &payload)
        return payload
    }

    public static func encodeContinue(id: UInt64) -> Data {
        var payload = Data(continueMagic)
        append(id, to: &payload)
        return payload
    }

    public static func encodeCancel(id: UInt64) -> Data {
        var payload = Data(cancelMagic)
        append(id, to: &payload)
        return payload
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
        if magic == fileMagic {
            guard let path = takeString(&offset, from: payload),
                  let size: UInt64 = take(&offset, from: payload) else { return nil }
            let remaining = payload.count - offset
            guard size <= UInt64(remaining) else { return nil }
            let data = payload.subdata(in: offset..<(offset + Int(size)))
            return .response(
                id: id,
                result: .pathContents(PathContents(path: path, isDirectory: false, data: data)))
        }
        if magic == listMagic {
            guard let path = takeString(&offset, from: payload),
                  let flags: UInt32 = take(&offset, from: payload),
                  let count: UInt32 = take(&offset, from: payload) else { return nil }
            var entries: [DirEntry] = []
            entries.reserveCapacity(Int(count))
            for _ in 0..<count {
                guard let entry = takeEntry(&offset, from: payload) else { return nil }
                entries.append(entry)
            }
            return .listing(
                id: id,
                contents: PathContents(path: path, isDirectory: true, entries: entries),
                hasMore: (flags & listHasMore) != 0,
                nameTruncated: (flags & listNameTruncated) != 0)
        }
        if magic == pathStatMagic {
            guard let path = takeString(&offset, from: payload),
                  let mode: UInt32 = take(&offset, from: payload),
                  let uid: UInt32 = take(&offset, from: payload),
                  let gid: UInt32 = take(&offset, from: payload),
                  let size: UInt64 = take(&offset, from: payload),
                  let mtime: Int64 = take(&offset, from: payload) else { return nil }
            return .response(
                id: id,
                result: .pathStat(
                    PathStat(path: path, mode: mode, uid: uid, gid: gid, size: size, mtime: mtime)))
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
        return GuestInfo(
            agentVersion: agentVersion,
            kernelRelease: kernelRelease,
            distroName: distroName,
            distroVersion: distroVersion,
            initSystem: initSystem,
            capabilities: capabilities)
    }

    private static func takeEntry(_ offset: inout Int, from data: Data) -> DirEntry? {
        guard offset + 1 < data.count else { return nil }
        let dtype = data[offset]
        let eflags = data[offset + 1]
        offset += 2
        guard let name = takeString(&offset, from: data) else { return nil }
        return DirEntry(
            name: name,
            fileType: DirEntryType(rawValue: dtype) ?? .unknown,
            continuesName: (eflags & entryContinuesName) != 0,
            nameIncomplete: (eflags & entryNameIncomplete) != 0)
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
