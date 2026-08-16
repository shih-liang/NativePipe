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
        let data = try Data(contentsOf: url)
        guard let ver = extract(from: data) else { throw Failure.missingTag }
        let trimmed = ver.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { throw Failure.emptyVersion }
        return trimmed
    }
}
