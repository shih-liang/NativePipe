import AppKit
import ImageIO
import UniformTypeIdentifiers
@testable import NativePipeSharing
import XCTest

final class ContentFormatsTests: XCTestCase {
    private func pasteboard(_ fill: (NSPasteboard) -> Void) -> NSPasteboard {
        let board = NSPasteboard.withUniqueName()
        board.clearContents()
        fill(board)
        return board
    }

    func testMacTextIsOfferedInEveryWaylandAndX11TextForm() throws {
        let board = pasteboard { $0.setString("Grüße 你好", forType: .string) }
        defer { board.releaseGlobally() }
        XCTAssertEqual(ContentFormats.mimeTypes(for: board),
                       ["text/plain;charset=utf-8", "UTF8_STRING", "text/plain", "TEXT", "STRING"])
        for mime in ["text/plain;charset=utf-8", "UTF8_STRING", "text/plain", "TEXT"] {
            XCTAssertEqual(ContentFormats.data(forMIME: mime, from: board), Data("Grüße 你好".utf8))
        }
        // STRING is Latin-1 (ICCCM); characters outside it are replaced.
        let latin1 = try XCTUnwrap(ContentFormats.data(forMIME: "STRING", from: board))
        XCTAssertEqual(String(data: latin1, encoding: .isoLatin1)?.hasPrefix("Grüße "), true)
    }

    func testGuestTextAndLatin1StringBecomeMacText() {
        let item = NSPasteboardItem()
        XCTAssertTrue(ContentFormats.write(Data("Grüße".utf8), mime: "text/plain;charset=utf-8", to: item))
        XCTAssertEqual(item.string(forType: .string), "Grüße")
        let legacy = NSPasteboardItem()
        XCTAssertTrue(ContentFormats.write(Data([0x47, 0x72, 0xFC, 0xDF, 0x65]), mime: "STRING", to: legacy))
        XCTAssertEqual(legacy.string(forType: .string), "Grüße")
    }

    func testHTMLKeepsAPlainTextCompanionAndAcceptsChromiumUTF16() {
        XCTAssertEqual(ContentFormats.preferredGuestTypes(from: ["text/plain", "text/html", "UTF8_STRING"]),
                       ["text/html", "UTF8_STRING"])
        let item = NSPasteboardItem()
        var utf16 = Data([0xFF, 0xFE]); utf16.append("<b>hi</b>".data(using: .utf16LittleEndian)!)
        XCTAssertTrue(ContentFormats.write(utf16, mime: "text/html", to: item))
        XCTAssertEqual(item.string(forType: .html), "<b>hi</b>")
    }

    func testWebLinksUseMozillaUTF16AndCRLFURIList() throws {
        let board = pasteboard {
            $0.writeObjects([URL(string: "https://example.com/a?b=1")! as NSURL])
            $0.setString("Example", forType: NSPasteboard.PasteboardType("public.url-name"))
        }
        defer { board.releaseGlobally() }
        let offered = ContentFormats.mimeTypes(for: board)
        XCTAssertEqual(Array(offered.prefix(2)), ["text/x-moz-url", "text/uri-list"])
        let moz = try XCTUnwrap(ContentFormats.data(forMIME: "text/x-moz-url", from: board))
        XCTAssertFalse(moz.starts(with: [0xFF, 0xFE]), "Firefox's format has no byte-order mark")
        XCTAssertEqual(String(data: moz, encoding: .utf16LittleEndian), "https://example.com/a?b=1\nExample")
        XCTAssertEqual(ContentFormats.data(forMIME: "text/uri-list", from: board), Data("https://example.com/a?b=1\r\n".utf8))

        let item = NSPasteboardItem()
        XCTAssertTrue(ContentFormats.write("https://example.org/\nTitle".data(using: .utf16LittleEndian)!,
                                           mime: "text/x-moz-url", to: item))
        XCTAssertEqual(item.string(forType: .URL), "https://example.org/")
        XCTAssertEqual(item.string(forType: NSPasteboard.PasteboardType("public.url-name")), "Title")
        XCTAssertFalse(ContentFormats.write(Data("file:///etc/hosts\r\n".utf8), mime: "text/uri-list", to: NSPasteboardItem()),
                       "File lists are shared through SharingSession, not as links")
    }

    func testImagesAreConvertedBetweenPNGAndTIFF() throws {
        let png = try XCTUnwrap(Self.pixelPNG())
        let board = pasteboard { $0.setData(png, forType: .png) }
        defer { board.releaseGlobally() }
        XCTAssertEqual(ContentFormats.data(forMIME: "image/png", from: board), png)
        for (mime, type) in [("image/tiff", UTType.tiff), ("image/jpeg", .jpeg), ("image/bmp", .bmp)] {
            let data = try XCTUnwrap(ContentFormats.data(forMIME: mime, from: board))
            XCTAssertEqual(Self.type(of: data), type.identifier)
        }
        let item = NSPasteboardItem()
        let jpeg = try XCTUnwrap(ContentFormats.data(forMIME: "image/jpeg", from: board))
        XCTAssertTrue(ContentFormats.write(jpeg, mime: "image/jpeg", to: item))
        XCTAssertEqual(Self.type(of: try XCTUnwrap(item.data(forType: .png))), UTType.png.identifier)
        XCTAssertEqual(Self.type(of: try XCTUnwrap(item.data(forType: .tiff))), UTType.tiff.identifier)
        XCTAssertEqual(ContentFormats.preferredGuestTypes(from: ["text/plain", "image/jpeg", "image/png"]), ["image/png"])
    }

    func testFilesAreFetchedAlone() {
        XCTAssertEqual(ContentFormats.preferredGuestTypes(from: ["text/plain;charset=utf-8", "text/uri-list"]), ["text/uri-list"])
    }

    private static func pixelPNG() -> Data? {
        let context = CGContext(data: nil, width: 2, height: 2, bitsPerComponent: 8, bytesPerRow: 0,
                                space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)
        context?.setFillColor(red: 1, green: 0, blue: 0, alpha: 1)
        context?.fill(CGRect(x: 0, y: 0, width: 2, height: 2))
        guard let image = context?.makeImage() else { return nil }
        let data = NSMutableData()
        guard let destination = CGImageDestinationCreateWithData(data, UTType.png.identifier as CFString, 1, nil) else { return nil }
        CGImageDestinationAddImage(destination, image, nil)
        return CGImageDestinationFinalize(destination) ? data as Data : nil
    }
    private static func type(of data: Data) -> String? {
        CGImageSourceCreateWithData(data as CFData, nil).flatMap { CGImageSourceGetType($0) as String? }
    }
}
