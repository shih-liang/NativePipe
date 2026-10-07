import FSKit
import NativePipeFileSystem

@available(macOS 27.0, *)
@main
struct NativePipeFileSystemExtension: UnaryFileSystemExtension {
    var fileSystem: SharedFilesFileSystem { SharedFilesFileSystem() }
}
