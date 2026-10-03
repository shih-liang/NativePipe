import Foundation

enum NativePipeResources {
    static let bundle: Bundle = {
        // Applications keep resources in Contents/Resources; a standalone
        // command keeps the same bundle beside its executable.
        let name = "NativePipe_NativePipeStrings.bundle"
        if let resources = Bundle.main.resourceURL,
           let bundle = Bundle(url: resources.appendingPathComponent(name)) {
            return bundle
        }
        if let bundle = Bundle(url: Bundle.main.bundleURL.appendingPathComponent(name)) {
            return bundle
        }
        // SwiftPM tests and development executables use the build product.
        return Bundle.module
    }()
}
