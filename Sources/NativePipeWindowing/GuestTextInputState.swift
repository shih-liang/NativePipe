import Foundation

/// AppKit indexes UTF-16; Wayland indexes UTF-8. Keep one validated snapshot
/// and a local preedit so reconversion never splits a scalar or deletes a
/// different word merely because the two index spaces have different lengths.
struct GuestTextInputState {
    struct Edit {
        var text: String
        var before: UInt32 = 0
        var after: UInt32 = 0
        var cursorBegin: Int = 0
        var cursorEnd: Int = 0
    }

    private(set) var surrounding: String?
    private var cursor = 0
    private var anchor = 0
    private(set) var markedText = ""
    private var markedSelection = NSRange(location: 0, length: 0)
    private(set) var hints: UInt32 = 0
    private(set) var purpose: UInt32 = 0

    var requiresRomanInput: Bool { hints & 0x100 != 0 || purpose == 8 || purpose == 9 }
    var isSensitive: Bool { hints & 0xc0 != 0 || purpose == 8 || purpose == 9 }

    static func utf8Offset(in text: String, utf16Offset: Int) -> Int? {
        guard utf16Offset >= 0, utf16Offset <= text.utf16.count else { return nil }
        let index = text.utf16.index(text.utf16.startIndex, offsetBy: utf16Offset)
        guard let scalar = index.samePosition(in: text.unicodeScalars),
              let bytes = scalar.samePosition(in: text.utf8) else { return nil }
        return text.utf8.distance(from: text.utf8.startIndex, to: bytes)
    }

    static func utf16Offset(in text: String, utf8Offset: Int) -> Int? {
        guard utf8Offset >= 0, utf8Offset <= text.utf8.count else { return nil }
        let index = text.utf8.index(text.utf8.startIndex, offsetBy: utf8Offset)
        guard let scalar = index.samePosition(in: text.unicodeScalars),
              let units = scalar.samePosition(in: text.utf16) else { return nil }
        return text.utf16.distance(from: text.utf16.startIndex, to: units)
    }

    mutating func updateSurrounding(_ text: String, cursor: Int, anchor: Int) -> Bool {
        guard text.utf8.count <= 4000, !text.contains("\0"),
              Self.utf16Offset(in: text, utf8Offset: cursor) != nil,
              Self.utf16Offset(in: text, utf8Offset: anchor) != nil else { return false }
        guard !isSensitive else { surrounding = nil; self.cursor = 0; self.anchor = 0; return true }
        surrounding = text
        self.cursor = cursor
        self.anchor = anchor
        return true
    }

    mutating func setContentType(hints: UInt32, purpose: UInt32) {
        self.hints = hints
        self.purpose = purpose
        if isSensitive { surrounding = nil; cursor = 0; anchor = 0 }
    }

    mutating func clearMarkedText() { markedText = ""; markedSelection = NSRange(location: 0, length: 0) }

    private var baseSelection: NSRange {
        guard let surrounding,
              let a = Self.utf16Offset(in: surrounding, utf8Offset: cursor),
              let b = Self.utf16Offset(in: surrounding, utf8Offset: anchor) else {
            return NSRange(location: 0, length: 0)
        }
        return NSRange(location: min(a, b), length: abs(a - b))
    }

    var selectedRange: NSRange {
        guard !markedText.isEmpty else { return baseSelection }
        guard markedSelection.location != NSNotFound else { return NSRange(location: NSNotFound, length: 0) }
        return NSRange(location: baseSelection.location + markedSelection.location, length: markedSelection.length)
    }

    var markedRange: NSRange {
        markedText.isEmpty ? NSRange(location: NSNotFound, length: 0)
            : NSRange(location: baseSelection.location, length: markedText.utf16.count)
    }

    private var document: String {
        let text = surrounding ?? ""
        return markedText.isEmpty ? text : (text as NSString).replacingCharacters(in: baseSelection, with: markedText)
    }

    func substring(in range: NSRange) -> String? {
        guard surrounding != nil, !isSensitive, range.location >= 0, range.length >= 0, range.location != NSNotFound,
              range.length <= document.utf16.count, range.location <= document.utf16.count - range.length,
              Self.utf8Offset(in: document, utf16Offset: range.location) != nil,
              Self.utf8Offset(in: document, utf16Offset: range.location + range.length) != nil else { return nil }
        return (document as NSString).substring(with: range)
    }

    /// delete_surrounding_text excludes the selection and the existing preedit.
    /// A range disjoint from the caret cannot be represented by that protocol.
    private func replacement(_ range: NSRange) -> (before: UInt32, after: UInt32)? {
        if range.location == NSNotFound { return (0, 0) }
        let text = document
        guard range.location >= 0, range.length >= 0,
              range.length <= text.utf16.count, range.location <= text.utf16.count - range.length,
              Self.utf8Offset(in: text, utf16Offset: range.location) != nil,
              Self.utf8Offset(in: text, utf16Offset: range.location + range.length) != nil else { return nil }
        let selected = baseSelection
        if !markedText.isEmpty, range.location >= markedRange.location,
           range.location + range.length <= markedRange.location + markedRange.length { return (0, 0) }
        let end = range.location + range.length
        let originalEnd = markedText.isEmpty ? end : end + selected.length - markedText.utf16.count
        guard surrounding != nil, range.location <= selected.location,
              originalEnd >= selected.location + selected.length,
              let startBytes = Self.utf8Offset(in: surrounding!, utf16Offset: range.location),
              let endBytes = Self.utf8Offset(in: surrounding!, utf16Offset: originalEnd) else { return nil }
        return (UInt32(min(cursor, anchor) - startBytes), UInt32(endBytes - max(cursor, anchor)))
    }

    private func replacingMarkedPart(_ text: String, range: NSRange) -> (String, Int) {
        guard !markedText.isEmpty, range.location != NSNotFound,
              range.location >= markedRange.location,
              range.location + range.length <= markedRange.location + markedRange.length else { return (text, 0) }
        let local = NSRange(location: range.location - markedRange.location, length: range.length)
        return ((markedText as NSString).replacingCharacters(in: local, with: text), local.location)
    }

    private mutating func deleteBase(before: UInt32, after: UInt32) {
        guard let surrounding else { return }
        let start = min(cursor, anchor) - Int(before)
        let end = max(cursor, anchor) + Int(after)
        let lower = Self.utf16Offset(in: surrounding, utf8Offset: start)!
        let upper = Self.utf16Offset(in: surrounding, utf8Offset: end)!
        self.surrounding = (surrounding as NSString).replacingCharacters(
            in: NSRange(location: lower, length: upper - lower), with: "")
        cursor = start; anchor = start
    }

    mutating func preedit(_ text: String, selection: NSRange, replacing range: NSRange) -> Edit? {
        guard let deletion = replacement(range) else { return nil }
        let (updated, offset) = replacingMarkedPart(text, range: range)
        var selected = selection
        if selected.location != NSNotFound {
            guard selected.location >= 0, selected.length >= 0, selected.length <= text.utf16.count,
                  selected.location <= text.utf16.count - selected.length else { return nil }
            selected.location += offset
        }
        let begin = selected.location == NSNotFound ? -1 : Self.utf8Offset(in: updated, utf16Offset: selected.location)
        let end = selected.location == NSNotFound ? -1 : Self.utf8Offset(in: updated, utf16Offset: selected.location + selected.length)
        guard let begin, let end else { return nil }
        deleteBase(before: deletion.before, after: deletion.after)
        markedText = updated; markedSelection = selected
        return Edit(text: updated, before: deletion.before, after: deletion.after, cursorBegin: begin, cursorEnd: end)
    }

    mutating func commit(_ text: String, replacing range: NSRange) -> Edit? {
        guard let deletion = replacement(range) else { return nil }
        let (updated, _) = replacingMarkedPart(text, range: range)
        deleteBase(before: deletion.before, after: deletion.after)
        if let surrounding, let offset = Self.utf16Offset(in: surrounding, utf8Offset: cursor) {
            self.surrounding = (surrounding as NSString).replacingCharacters(in: NSRange(location: offset, length: 0), with: updated)
            cursor += updated.utf8.count; anchor = cursor
        }
        clearMarkedText()
        return Edit(text: updated, before: deletion.before, after: deletion.after)
    }
}
