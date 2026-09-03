// swift-tools-version: 6.0
import PackageDescription
import Foundation

let repositoryRoot = Context.packageDirectory
let buildEnvironment = ProcessInfo.processInfo.environment
let virglPrefix = buildEnvironment["NATIVEPIPE_VIRGL_PREFIX"]
    ?? "\(repositoryRoot)/vendor/virglrenderer-prefix"
let moltenVKPrefix = buildEnvironment["NATIVEPIPE_MOLTENVK_PREFIX"]
    ?? "\(repositoryRoot)/vendor/moltenvk-prefix"
let anglePrefix = buildEnvironment["NATIVEPIPE_ANGLE_PREFIX"]
    ?? "\(repositoryRoot)/vendor/angle-prefix"
let omitGPU = buildEnvironment["NATIVEPIPE_OMIT_GPU"] == "1"
let rendererLinkSettings: [LinkerSetting] = [
    .unsafeFlags([
        "\(virglPrefix)/lib/libvirglrenderer.a",
        "\(anglePrefix)/lib/libepoxy.a",
        "\(moltenVKPrefix)/lib/libMoltenVK.a",
    ]),
    .linkedLibrary("c++"),
    .linkedFramework("AppKit"),
    .linkedFramework("CoreFoundation"),
    .linkedFramework("CoreGraphics"),
    .linkedFramework("CoreMedia"),
    .linkedFramework("CoreVideo"),
    .linkedFramework("Foundation"),
    .linkedFramework("IOKit"),
    .linkedFramework("IOSurface"),
    .linkedFramework("Metal"),
    .linkedFramework("QuartzCore"),
    .linkedFramework("VideoToolbox"),
]

var products: [Product] = [
    .library(name: "NativePipeProtocol", targets: ["NativePipeProtocol"]),
    .library(name: "NativePipeWindowing", targets: ["NativePipeWindowing"]),
    .library(name: "NativePipeRemote", targets: ["NativePipeRemote"]),
    .executable(name: "nativepipe", targets: ["NativePipeRemoteApp"]),
]
var targets: [Target] = [
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

// The standalone remote client has no renderer-SDK dependency. Repository CI
// explicitly omits the GPU-only targets; FluxWindow supplies all three SDKs and
// receives the normal manifest containing NativePipeGPU.
if !omitGPU {
    products.append(.library(name: "NativePipeGPU", targets: ["NativePipeGPU"]))
    targets.append(
        .target(
            name: "NativePipeVenus",
            publicHeadersPath: "include",
            cSettings: [.unsafeFlags(["-I\(virglPrefix)/include/virgl"])],
            linkerSettings: rendererLinkSettings
        )
    )
    targets.append(
        .target(
            name: "NativePipeGPU",
            dependencies: ["NativePipeVenus"],
            swiftSettings: [.swiftLanguageMode(.v5)]
        )
    )
    targets.append(
        .testTarget(
            name: "NativePipeGPUTests",
            dependencies: ["NativePipeGPU"],
            swiftSettings: [.swiftLanguageMode(.v5)]
        )
    )
}

/// NativePipe — shared display, input and transport technology for FluxWindow.
let package = Package(
    name: "NativePipe",
    platforms: [.macOS(.v14)],
    products: products,
    dependencies: [],
    targets: targets
)
