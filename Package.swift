// swift-tools-version: 6.0
import PackageDescription

let products: [Product] = [
    .library(name: "NativePipeProtocol", targets: ["NativePipeProtocol"]),
    .library(name: "NativePipeWindowing", targets: ["NativePipeWindowing"]),
    .library(name: "NativePipeRemote", targets: ["NativePipeRemote"]),
    .executable(name: "nativepipe", targets: ["NativePipeRemoteApp"]),
]
let targets: [Target] = [
        .target(
            name: "NativePipeProtocol",
            swiftSettings: [.swiftLanguageMode(.v5)]
        ),
        // Wayland guest events ↔ NSWindow / clipboard / key codes.
        .target(
            name: "NativePipeWindowing",
            dependencies: ["NativePipeProtocol"],
            swiftSettings: [.swiftLanguageMode(.v5)]
        ),
        // Remote Linux: TCP NPIP + NPEN, VideoToolbox decode, WindowBridge.
        .target(
            name: "NativePipeRemote",
            dependencies: ["NativePipeProtocol", "NativePipeWindowing"],
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
            dependencies: [
                "NativePipeRemote",
                "NativePipeProtocol",
            ],
            swiftSettings: [.swiftLanguageMode(.v5)]
        ),
        .testTarget(
            name: "NativePipeProtocolTests",
            dependencies: ["NativePipeProtocol"],
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
            swiftSettings: [.swiftLanguageMode(.v5)]
        ),
        .testTarget(
            name: "NativePipeRemoteAppTests",
            dependencies: ["NativePipeRemoteApp"],
            swiftSettings: [.swiftLanguageMode(.v5)]
        ),
]

/// NativePipe — shared display, input and transport technology for FluxWindow.
let package = Package(
    name: "NativePipe",
    platforms: [.macOS(.v14)],
    products: products,
    dependencies: [],
    targets: targets
)
