import AppKit
import Foundation
import XCTest
import Metal
import QuartzCore
import NativePipeProtocol
@testable import NativePipeWindowing

final class GatedLayer: CAMetalLayer, @unchecked Sendable {
    private let state = NSLock()
    private var entered = false
    private var calls = 0
    private var successes = 0
    let resume = DispatchSemaphore(value: 0)

    var isWaiting: Bool {
        state.lock(); defer { state.unlock() }
        return entered
    }
    var drawableCount: Int {
        state.lock(); defer { state.unlock() }
        return successes
    }
    override func nextDrawable() -> CAMetalDrawable? {
        state.lock()
        calls += 1
        let first = calls == 1
        if first { entered = true }
        state.unlock()
        if first { _ = resume.wait(timeout: .now() + 5) }
        let result = super.nextDrawable()
        if result != nil {
            state.lock(); successes += 1; state.unlock()
        }
        return result
    }
}

final class PresenterLatestTests: XCTestCase {
    @MainActor func testLatestSceneIsSelectedAfterDrawableWait() async throws {
        guard let device = MTLCreateSystemDefaultDevice() else {
            throw XCTSkip("Metal unavailable")
        }
        let layer = GatedLayer()
        layer.device = device
        layer.pixelFormat = .bgra8Unorm
        layer.drawableSize = CGSize(width: 128, height: 96)
        let renderer = try HostSceneRenderer(device: device)
        let presenter = AsyncMetalScenePresenter(layer: layer, device: device, renderer: renderer)
        var submitted: [Int] = []
        var readSuccess: [Int] = []
        var discarded: [Int] = []
        func enqueue(_ index: Int) {
            let descriptor = MTLTextureDescriptor.texture2DDescriptor(
                pixelFormat: .bgra8Unorm, width: 128, height: 96, mipmapped: false)
            descriptor.storageMode = .shared
            descriptor.usage = [.shaderRead]
            let texture = device.makeTexture(descriptor: descriptor)!
            let bytes = [UInt8](repeating: UInt8(index * 20), count: 128 * 96 * 4)
            bytes.withUnsafeBytes { raw in
                texture.replace(region: MTLRegionMake2D(0, 0, 128, 96), mipmapLevel: 0,
                    withBytes: raw.baseAddress!, bytesPerRow: 128 * 4)
            }
            let state = Windowing.SceneLayer(
                surface: 1, resourceID: UInt32(index), width: 128, height: 96,
                bytesPerRow: 128 * 4, format: .bgrx8888,
                destination: .init(x: 0, y: 0, width: 128, height: 96),
                sourcePixels: .init(x: 0, y: 0, width: 128, height: 96),
                clip: .init(x: 0, y: 0, width: 128, height: 96))
            let scene = Windowing.SceneSnapshot(surface: 1, presentationID: UInt32(index),
                width: 128, height: 96, scale: 1,
                windowGeometry: .init(x: 0, y: 0, width: 128, height: 96),
                layers: [state], damage: [.init(x: 0, y: 0, width: 128, height: 96)])
            presenter.enqueue(scene: scene, layers: [.init(state: state, texture: texture)],
                drawableSize: CGSize(width: 128, height: 96),
                readComplete: { success in
                    if success { readSuccess.append(index) } else { discarded.append(index) }
                }, latched: { submitted.append(index) })
        }
        enqueue(1)
        for _ in 0..<300 {
            if layer.isWaiting { break }
            try await Task.sleep(for: .milliseconds(5))
        }
        guard layer.isWaiting else {
            XCTFail("Presenter did not acquire drawable"); return
        }
        for index in 2...8 { enqueue(index) }
        layer.resume.signal()
        for _ in 0..<600 {
            if readSuccess.count + discarded.count == 8 { break }
            try await Task.sleep(for: .milliseconds(5))
        }
        XCTAssertEqual(readSuccess, [8])
        XCTAssertEqual(discarded.sorted(), Array(1...7))
        XCTAssertEqual(submitted.sorted(), Array(1...8), "Superseding keeps all latch obligations")
        XCTAssertEqual(layer.drawableCount, 1)
        presenter.cancelPending()
    }
}
