import AppKit
import ImageIO
import UniformTypeIdentifiers

/// Converts between macOS pasteboard types and the MIME types Wayland (and
/// X11 through Xwayland) clients exchange, for the clipboard and drag and drop.
///
/// Conventions followed:
/// - Text: GTK serializes strings as `text/plain;charset=utf-8` and
///   `text/plain`; Qt sends `text/plain` as UTF-8; X11 clients use
///   `UTF8_STRING`, `TEXT` and the Latin-1 `STRING`.
/// - Links: `text/uri-list` lines end in CRLF (RFC 2483); Firefox and Chromium
///   add `text/x-moz-url`, UTF-16LE without a byte-order mark, "url\ntitle".
/// - Images: Qt re-encodes to whichever `image/*` type is requested; Mac apps
///   read PNG or TIFF, so other encodings are converted.
/// - Files travel as `text/uri-list` of `file:` URLs only. Which files those
///   are is decided by SharingSession; this type never touches file contents.
public enum ContentFormats {
    public static let fileList = "text/uri-list"
    public static let mozURL = "text/x-moz-url"
    static let utf8Text = ["text/plain;charset=utf-8", "UTF8_STRING", "text/plain", "TEXT"]
    static let latin1Text = "STRING"
    static let textTypes = utf8Text + [latin1Text]
    static let imageTypes: [(mime: String, type: UTType)] = [
        ("image/png", .png), ("image/tiff", .tiff), ("image/jpeg", .jpeg), ("image/bmp", .bmp)]
    static let urlName = NSPasteboard.PasteboardType("public.url-name")

    // MARK: Mac → Linux

    /// What a Mac pasteboard holds, as Wayland MIME types, best first.
    /// `text/uri-list` is listed for file URLs and for web links.
    public static func mimeTypes(for pasteboard: NSPasteboard) -> [String] {
        let types = Set(pasteboard.types ?? [])
        var mimes: [String] = []
        if hasFiles(pasteboard) { mimes.append(fileList) }
        else if webURL(in: pasteboard) != nil { mimes += [mozURL, fileList] }
        if types.contains(.html) { mimes.append("text/html") }
        if types.contains(.rtf) { mimes.append("text/rtf") }
        if types.contains(.png) || types.contains(.tiff) { mimes += imageTypes.map(\.mime) }
        if types.contains(.string) || webURL(in: pasteboard) != nil { mimes += textTypes }
        return mimes
    }

    public static func hasFiles(_ pasteboard: NSPasteboard) -> Bool {
        pasteboard.canReadObject(forClasses: [NSURL.self], options: [.urlReadingFileURLsOnly: true])
    }

    /// Bytes for one MIME type from a Mac pasteboard; files are not handled here.
    public static func data(forMIME mime: String, from pasteboard: NSPasteboard) -> Data? {
        if textTypes.contains(mime) {
            guard let text = pasteboard.string(forType: .string) ?? webURL(in: pasteboard)?.url.absoluteString else { return nil }
            return mime == latin1Text ? text.data(using: .isoLatin1, allowLossyConversion: true) : Data(text.utf8)
        }
        switch mime {
        case "text/html":
            return pasteboard.string(forType: .html).map { Data($0.utf8) } ?? pasteboard.data(forType: .html)
        case "text/rtf":
            return pasteboard.data(forType: .rtf)
        case mozURL:
            guard let link = webURL(in: pasteboard) else { return nil }
            return (link.url.absoluteString + "\n" + (link.title ?? link.url.absoluteString)).data(using: .utf16LittleEndian)
        case fileList:
            return webURL(in: pasteboard).map { Data(($0.url.absoluteString + "\r\n").utf8) }
        default:
            guard let target = imageTypes.first(where: { $0.mime == mime })?.type else { return nil }
            if target == .png, let png = pasteboard.data(forType: .png) { return png }
            if target == .tiff, let tiff = pasteboard.data(forType: .tiff) { return tiff }
            guard let source = pasteboard.data(forType: .png) ?? pasteboard.data(forType: .tiff) else { return nil }
            return convertImage(source, to: target)
        }
    }

    // MARK: Linux → Mac

    /// The MIME types to fetch from a guest selection, best first: the richest
    /// representation, plus plain text beside rich text so plain editors can
    /// paste it. Files and images are fetched alone.
    public static func preferredGuestTypes(from offered: [String]) -> [String] {
        let offered = Set(offered)
        let text = textTypes.first(where: offered.contains)
        if offered.contains(fileList) { return [fileList] }
        if offered.contains(mozURL) { return [mozURL] + (text.map { [$0] } ?? []) }
        if let image = imageTypes.first(where: { offered.contains($0.mime) }) { return [image.mime] }
        for rich in ["text/html", "text/rtf"] where offered.contains(rich) { return [rich] + (text.map { [$0] } ?? []) }
        return text.map { [$0] } ?? []
    }

    /// Writes guest bytes to a Mac pasteboard item. Returns false for content
    /// this type does not handle (files, unknown types, undecodable bytes).
    @discardableResult
    public static func write(_ data: Data, mime: String, to item: NSPasteboardItem) -> Bool {
        if textTypes.contains(mime) {
            guard let text = decodeText(data, latin1: mime == latin1Text) else { return false }
            return item.setString(text, forType: .string)
        }
        switch mime {
        case "text/html":
            guard let html = decodeText(data, latin1: false) else { return false }
            return item.setString(html, forType: .html)
        case "text/rtf":
            return item.setData(data, forType: .rtf)
        case mozURL:
            let text = String(data: data, encoding: .utf16LittleEndian) ?? decodeText(data, latin1: false) ?? ""
            let lines = text.split(whereSeparator: \.isNewline).map(String.init)
            guard let first = lines.first, let url = URL(string: first), url.scheme != nil else { return false }
            item.setString(url.absoluteString, forType: .URL)
            if lines.count > 1 { item.setString(lines[1], forType: urlName) }
            return true
        case fileList:
            // Web links only; files are shared through SharingSession.
            let lines = (decodeText(data, latin1: false) ?? "").split(whereSeparator: \.isNewline).filter { !$0.hasPrefix("#") }
            guard let first = lines.first, let url = URL(string: String(first)), url.scheme != nil, !url.isFileURL else { return false }
            return item.setString(url.absoluteString, forType: .URL)
        default:
            guard imageTypes.contains(where: { $0.mime == mime }) else { return false }
            guard let png = mime == "image/png" ? data : convertImage(data, to: .png) else { return false }
            // Mac apps read PNG or TIFF; offer both.
            item.setData(png, forType: .png)
            if let tiff = convertImage(png, to: .tiff) { item.setData(tiff, forType: .tiff) }
            return true
        }
    }

    // MARK: Helpers

    /// A non-file URL on the pasteboard with its title, if any.
    public static func webURL(in pasteboard: NSPasteboard) -> (url: URL, title: String?)? {
        guard let url = (pasteboard.readObjects(forClasses: [NSURL.self]) as? [URL])?.first,
              !url.isFileURL, url.scheme != nil else { return nil }
        return (url, pasteboard.string(forType: urlName))
    }

    static func decodeText(_ data: Data, latin1: Bool) -> String? {
        if data.starts(with: [0xFF, 0xFE]) { return String(data: data.dropFirst(2), encoding: .utf16LittleEndian) }
        if data.starts(with: [0xFE, 0xFF]) { return String(data: data.dropFirst(2), encoding: .utf16BigEndian) }
        if latin1 { return String(data: data, encoding: .isoLatin1) }
        return String(data: data, encoding: .utf8)
    }

    static func convertImage(_ data: Data, to type: UTType) -> Data? {
        guard let source = CGImageSourceCreateWithData(data as CFData, nil),
              let image = CGImageSourceCreateImageAtIndex(source, 0, nil) else { return nil }
        let output = NSMutableData()
        guard let destination = CGImageDestinationCreateWithData(output, type.identifier as CFString, 1, nil) else { return nil }
        CGImageDestinationAddImage(destination, image, nil)
        return CGImageDestinationFinalize(destination) ? output as Data : nil
    }
}
