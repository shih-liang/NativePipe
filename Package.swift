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
        .library(name: "NativePipeGPU", targets: ["NativePipeGPU"]),
    ],
    targets: [
        .target(
            name: "NativePipeProtocol",
            swiftSettings: [.swiftLanguageMode(.v5)]
        ),
        // Host side of the virtual GPU: dlopen virglrenderer, advertise the
        // Venus capset, forward SUBMIT_3D. Venus itself is guest Mesa.
        .target(
            name: "NativePipeVenus",
            publicHeadersPath: "include",
            linkerSettings: [
                .linkedFramework("CoreFoundation"),
                .linkedFramework("IOSurface"),
            ]
        ),
        // Standalone on purpose: the GPU device talks to Virtualization, Metal
        // and IOSurface, and knows nothing about VM bundles or the guest agent.
        .target(
            name: "NativePipeGPU",
            dependencies: ["NativePipeVenus"],
            swiftSettings: [.swiftLanguageMode(.v5)]
        ),
        // Wayland guest events ↔ NSWindow / clipboard / key codes.
        .target(
            name: "NativePipeWindowing",
            dependencies: ["NativePipeProtocol"],
            swiftSettings: [.swiftLanguageMode(.v5)]
        ),
    ]
)
