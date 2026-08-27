// swift-tools-version: 6.0
import PackageDescription

/// NativePipe — Wayland↔macOS display bridge.
///
/// Reusable without Virtualization: remote real-machine hosts depend only on
/// this package. LightHouse (VM management) depends on it from the repo root.
let package = Package(
    name: "NativePipe",
    platforms: [.macOS(.v14)],
    products: [
        .library(name: "NativePipeProtocol", targets: ["NativePipeProtocol"]),
        .library(name: "NativePipeWindowing", targets: ["NativePipeWindowing"]),
        .library(name: "NativePipeRemote", targets: ["NativePipeRemote"]),
        .executable(name: "remotepipe", targets: ["NativePipeRemoteApp"]),
    ],
    dependencies: [
        .package(url: "https://github.com/apple/swift-argument-parser.git", from: "1.5.0"),
    ],
    targets: [
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
    ]
)
