// swift-tools-version: 6.0
import PackageDescription
import Foundation

let codecPrefix = URL(fileURLWithPath: #filePath).deletingLastPathComponent().appendingPathComponent(".build/codecs/macos").path

let products: [Product] = [
    .library(name: "NativePipeProtocol", targets: ["NativePipeProtocol"]),
    .library(name: "NativePipeWindowing", targets: ["NativePipeWindowing"]),
    .library(name: "NativePipeRemote", targets: ["NativePipeRemote"]),
    .library(name: "NativePipeFileSharing", targets: ["NativePipeFileSharing"]),
    .library(name: "NativePipeFileSystem", targets: ["NativePipeFileSystem"]),
    .executable(name: "NativePipeFileSystemExtension", targets: ["NativePipeFileSystemExtension"]),
    .executable(name: "nativepipe", targets: ["NativePipeRemoteApp"]),
]
// The display CLI continues to support macOS 14. FSKit is optional at launch;
// only filesystem publication and the extension require macOS 27.
let fsKitLink: [LinkerSetting] = [.unsafeFlags(["-Xlinker", "-weak_framework", "-Xlinker", "FSKit"])]
let targets: [Target] = [
        .target(name: "NativePipeFileSharing", dependencies: ["NativePipeStrings", "NativePipeProtocol"],
                swiftSettings: [.swiftLanguageMode(.v5)], linkerSettings: fsKitLink),
        .target(name: "NativePipeFileSystem", dependencies: ["NativePipeFileSharing"],
                swiftSettings: [.swiftLanguageMode(.v5)], linkerSettings: fsKitLink),
        .executableTarget(name: "NativePipeFileSystemExtension", dependencies: ["NativePipeFileSystem"],
                swiftSettings: [.swiftLanguageMode(.v5)], linkerSettings: fsKitLink),
        .testTarget(name: "NativePipeFileSharingTests", dependencies: ["NativePipeFileSharing", "NativePipeProtocol"],
                swiftSettings: [.swiftLanguageMode(.v5)], linkerSettings: fsKitLink),
        .testTarget(name: "NativePipeFileSystemTests", dependencies: ["NativePipeFileSystem", "NativePipeFileSharing", "NativePipeProtocol"],
                swiftSettings: [.swiftLanguageMode(.v5)], linkerSettings: fsKitLink),
        .testTarget(name: "NativePipeStringsTests", dependencies: ["NativePipeStrings"], swiftSettings: [.swiftLanguageMode(.v5)]),
        .target(name: "NativePipeStrings", resources: [.process("Resources")], swiftSettings: [.swiftLanguageMode(.v5)]),
        .target(name: "CNativePipeAV1", path: "common/av1_decoder", publicHeadersPath: ".",
            cSettings: [.unsafeFlags(["-I" + codecPrefix + "/include"])],
            linkerSettings: [.unsafeFlags(["-L" + codecPrefix + "/lib"]),
                .linkedLibrary("dav1d"), .linkedLibrary("yuv"), .linkedLibrary("c++"),
                .linkedFramework("CoreVideo")]),
        .target(
            name: "CNativePipeFileRPC",
            path: "common/file_rpc",
            exclude: ["Makefile", "README.md", "tests.c", "build"],
            sources: ["np_file_wire.c", "np_file_server.c", "np_file_service.c"],
            publicHeadersPath: "."
        ),
        .target(
            name: "NativePipeProtocol",
            dependencies: ["NativePipeStrings", "CNativePipeFileRPC"],
            swiftSettings: [.swiftLanguageMode(.v5)]
        ),
        // Wayland guest events ↔ NSWindow / clipboard / key codes.
        .target(
            name: "NativePipeWindowing",
            dependencies: ["NativePipeStrings", "NativePipeProtocol"],
            swiftSettings: [.swiftLanguageMode(.v5)]
        ),
        // Remote Linux: SSH stdio NPIP + NPEN, VideoToolbox decode, WindowBridge.
        .target(
            name: "NativePipeRemote",
            dependencies: ["NativePipeStrings", "NativePipeProtocol", "NativePipeWindowing", "CNativePipeAV1"],
            swiftSettings: [.swiftLanguageMode(.v5)],
            linkerSettings: [
                .linkedFramework("VideoToolbox"),
                .linkedFramework("CoreMedia"),
                .linkedFramework("CoreVideo"),
                .linkedFramework("IOSurface"),
                .linkedFramework("AppKit"),
            ]
        ),
        .executableTarget(
            name: "NativePipeRemoteApp",
            dependencies: ["NativePipeStrings", "NativePipeFileSharing", "NativePipeWindowing",
                "NativePipeRemote",
                "NativePipeProtocol",
            ],
            swiftSettings: [.swiftLanguageMode(.v5)]
        ),
        .testTarget(
            name: "NativePipeProtocolTests",
            dependencies: ["NativePipeProtocol", "CNativePipeFileRPC"],
            swiftSettings: [.swiftLanguageMode(.v5)]
        ),
        .testTarget(
            name: "NativePipeWindowingTests",
            dependencies: ["NativePipeWindowing", "NativePipeProtocol"],
            swiftSettings: [.swiftLanguageMode(.v5)]
        ),
        .testTarget(
            name: "NativePipeRemoteTests",
            dependencies: ["NativePipeRemote", "NativePipeProtocol"],
            resources: [.copy("Resources")],
            swiftSettings: [.swiftLanguageMode(.v5)]
        ),
        .testTarget(
            name: "NativePipeRemoteAppTests",
            dependencies: ["NativePipeRemoteApp"],
            swiftSettings: [.swiftLanguageMode(.v5)]
        ),
]

/// NativePipe — shared display, input and transport technology for LinPortal.
let package = Package(
    name: "NativePipe",
    defaultLocalization: "en",
    platforms: [.macOS(.v14)],
    products: products,
    dependencies: [],
    targets: targets
)
