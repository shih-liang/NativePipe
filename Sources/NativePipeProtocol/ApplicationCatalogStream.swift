import Foundation

/// Local manager/launcher IPC must not re-pack a streamed catalog into one
/// bounded control frame. Each application is a binary-plist NPFR record;
/// an empty record terminates the catalog. There is no per-record roundtrip.
public enum ApplicationCatalogStream {
    public static func write(_ applications: [GuestApplication], to handle: FileHandle) throws {
        let encoder = PropertyListEncoder()
        encoder.outputFormat = .binary
        for application in applications {
            try FileRPC.sendRecord(encoder.encode(application), to: handle)
        }
        try FileRPC.sendRecord(Data(), to: handle)
    }

    public static func read(from handle: FileHandle) throws -> [GuestApplication] {
        let decoder = PropertyListDecoder()
        var applications: [GuestApplication] = []
        var ids = Set<String>()
        var metadataBytes = 0, iconBytes = 0
        while true {
            // Six bounded strings, one <=64-KiB PNG, and plist structure.
            let data = try FileRPC.receiveRecord(from: handle, maximum: 7 * 1024 * 1024)
            if data.isEmpty { return applications }
            let application = try decoder.decode(GuestApplication.self, from: data)
            guard !application.id.isEmpty, !application.id.contains("/"),
                  ids.insert(application.id).inserted, ids.count <= 65_536,
                  (application.iconData?.count ?? 0) <= 65_536 else {
                throw FileRPC.Failure.protocolError
            }
            metadataBytes += application.metadataByteCount
            iconBytes += application.iconData?.count ?? 0
            // Same memory policy as the remote-facing ApplicationClient, not
            // an extra limit on their combined serialized transport size.
            guard metadataBytes <= ApplicationClient.maximumMetadataBytes,
                  iconBytes <= ApplicationClient.maximumIconBytes else {
                throw FileRPC.Failure.protocolError
            }
            applications.append(application)
        }
    }
}

extension GuestApplication {
    var metadataByteCount: Int {
        id.utf8.count + name.utf8.count + executable.utf8.count
            + (comment?.utf8.count ?? 0) + (startupWMClass?.utf8.count ?? 0)
            + (iconName?.utf8.count ?? 0)
    }
}
