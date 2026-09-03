import Foundation

/// Version stamped into the static `nativepipe-guestd` ELF at build time.
///
/// The guest binary contains the ASCII marker `NPGV:<semver>\0`. Host code
/// must read the version from the bytes it actually serves — never trust a
/// sidecar `VERSION` file alone.
public enum GuestdBinaryVersion {
    /// Prefix of the embedded record (must match `nativepipe-guestd.c`).
    public static let tagPrefix = "NPGV:"

    public enum Failure: LocalizedError {
        case missingTag
        case emptyVersion
        case versionMismatch(file: String, elf: String)

        public var errorDescription: String? {
            switch self {
            case .missingTag:
                return "guestd binary has no \(tagPrefix) version stamp (rebuild with make -C guest/guestd)"
            case .emptyVersion:
                return "guestd binary version stamp is empty"
            case .versionMismatch(let file, let elf):
                return "VERSION file (\(file)) does not match ELF \(tagPrefix)\(elf)"
            }
        }
    }

    /// Scan raw ELF (or any blob) for `NPGV:<version>`.
    public static func extract(from data: Data) -> String? {
        let prefix = Data(tagPrefix.utf8)
        var search = data.startIndex
        while search < data.endIndex,
              let range = data.range(of: prefix, in: search..<data.endIndex)
        {
            let verStart = range.upperBound
            var verEnd = verStart
            while verEnd < data.endIndex {
                let b = data[verEnd]
                if b == 0 || b == UInt8(ascii: " ") || b == UInt8(ascii: "\n")
                    || b == UInt8(ascii: "\r") || b == UInt8(ascii: "\t")
                {
                    break
                }
                // Keep semver-ish characters only.
                let ok =
                    (b >= UInt8(ascii: "0") && b <= UInt8(ascii: "9"))
                    || (b >= UInt8(ascii: "a") && b <= UInt8(ascii: "z"))
                    || (b >= UInt8(ascii: "A") && b <= UInt8(ascii: "Z"))
                    || b == UInt8(ascii: ".") || b == UInt8(ascii: "-")
                    || b == UInt8(ascii: "+") || b == UInt8(ascii: "_")
                if !ok { break }
                verEnd += 1
            }
            if verEnd > verStart {
                if let s = String(data: data[verStart..<verEnd], encoding: .utf8), !s.isEmpty {
                    return s
                }
            }
            search = range.lowerBound.advanced(by: 1)
        }
        return nil
    }

    public static func extract(fromFile url: URL) throws -> String {
        let handle = try FileHandle(forReadingFrom: url)
        defer { try? handle.close() }
        return try validated(extract(from: handle))
    }

    /// Streaming variant used by the vsock file server. It keeps only the
    /// marker state and a bounded version token, even if the ELF is large.
    public static func extract(from handle: FileHandle) throws -> String {
        let prefix = Array(tagPrefix.utf8)
        var prefixIndex = 0
        var version = Data()
        var collecting = false
        while let chunk = try handle.read(upToCount: 64 << 10), !chunk.isEmpty {
            for byte in chunk {
                if collecting {
                    if versionByte(byte) {
                        guard version.count < 256 else { throw Failure.missingTag }
                        version.append(byte)
                        continue
                    }
                    if !version.isEmpty {
                        guard let value = String(data: version, encoding: .utf8) else {
                            throw Failure.missingTag
                        }
                        return value
                    }
                    collecting = false
                    prefixIndex = byte == prefix[0] ? 1 : 0
                    continue
                }

                if byte == prefix[prefixIndex] {
                    prefixIndex += 1
                    if prefixIndex == prefix.count {
                        prefixIndex = 0
                        collecting = true
                        version.removeAll(keepingCapacity: true)
                    }
                } else {
                    prefixIndex = byte == prefix[0] ? 1 : 0
                }
            }
        }
        if collecting, !version.isEmpty,
           let value = String(data: version, encoding: .utf8) {
            return value
        }
        throw Failure.missingTag
    }

    private static func versionByte(_ byte: UInt8) -> Bool {
        (byte >= UInt8(ascii: "0") && byte <= UInt8(ascii: "9"))
            || (byte >= UInt8(ascii: "a") && byte <= UInt8(ascii: "z"))
            || (byte >= UInt8(ascii: "A") && byte <= UInt8(ascii: "Z"))
            || byte == UInt8(ascii: ".") || byte == UInt8(ascii: "-")
            || byte == UInt8(ascii: "+") || byte == UInt8(ascii: "_")
    }

    private static func validated(_ ver: String) throws -> String {
        let trimmed = ver.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { throw Failure.emptyVersion }
        return trimmed
    }
}
