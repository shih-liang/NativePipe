import Foundation
import FSKit
import NativePipeFileSharing

/// FSKit transports the resource's directory grant to this sandbox. The directory
/// contains only a manifest and the broker socket; guest paths never cross it.
@available(macOS 27.0, *)
public final class SharedFilesFileSystem: FSUnaryFileSystem, FSUnaryFileSystemOperations {
    public override init() { super.init() }
    private var resource: FSPathURLResource?
    private let fileSystemTypeName = (Bundle.main.object(forInfoDictionaryKey: "EXAppExtensionAttributes")
        as? [String: Any])?["FSShortName"] as? String ?? "nativepipe"

    public func probeResource(resource: FSResource, replyHandler: @escaping (FSProbeResult?, Error?) -> Void) {
        guard let path = resource as? FSPathURLResource,
              path.url.startAccessingSecurityScopedResource() else {
            return replyHandler(nil, POSIXError(.EACCES))
        }
        defer { path.url.stopAccessingSecurityScopedResource() }
        do {
            let descriptor = try SharedFileVolumeClient(directory: path.url).descriptor
            replyHandler(.usable(name: descriptor.name,
                containerID: FSContainerIdentifier(uuid: descriptor.id)), nil)
        } catch { replyHandler(nil, error) }
    }

    public func loadResource(resource: FSResource, options: FSTaskOptions,
                      replyHandler: @escaping (FSVolume?, Error?) -> Void) {
        guard self.resource == nil else { return replyHandler(nil, POSIXError(.EBUSY)) }
        guard !options.taskOptions.contains("-f") else { return replyHandler(nil, POSIXError(.ENOTSUP)) }
        guard let path = resource as? FSPathURLResource,
              path.url.startAccessingSecurityScopedResource() else {
            return replyHandler(nil, POSIXError(.EACCES))
        }
        do {
            let client = try SharedFileVolumeClient(directory: path.url)
            self.resource = path
            containerStatus = .ready
            replyHandler(SharedFilesVolume(client: client, fileSystemTypeName: fileSystemTypeName), nil)
        } catch {
            path.url.stopAccessingSecurityScopedResource()
            replyHandler(nil, error)
        }
    }

    public func unloadResource(resource: FSResource, options: FSTaskOptions,
                        replyHandler: @escaping (Error?) -> Void) {
        guard let path = resource as? FSPathURLResource,
              let loaded = self.resource, loaded.url == path.url else {
            return replyHandler(POSIXError(.EINVAL))
        }
        self.resource = nil
        loaded.url.stopAccessingSecurityScopedResource()
        replyHandler(nil)
    }

    deinit { resource?.url.stopAccessingSecurityScopedResource() }
}
