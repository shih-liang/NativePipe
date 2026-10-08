import Foundation
@testable import NativePipeWindowing
import XCTest

final class GuestTextInputStateTests: XCTestCase {
    func testScalarBoundariesAcrossBothIndexSpaces() {
        let text = "a中😀e\u{301}"
        for (bytes, units) in [(0, 0), (1, 1), (4, 2), (8, 4), (9, 5), (11, 6)] {
            XCTAssertEqual(GuestTextInputState.utf16Offset(in: text, utf8Offset: bytes), units)
            XCTAssertEqual(GuestTextInputState.utf8Offset(in: text, utf16Offset: units), bytes)
        }
        XCTAssertNil(GuestTextInputState.utf16Offset(in: text, utf8Offset: 2))
        XCTAssertNil(GuestTextInputState.utf8Offset(in: text, utf16Offset: 3))
        XCTAssertNil(GuestTextInputState.utf8Offset(in: text, utf16Offset: -1))
    }

    func testSelectionAndSurroundingSubstringUseUTF16() throws {
        var state = GuestTextInputState()
        XCTAssertTrue(state.updateSurrounding("ab中😀z", cursor: 5, anchor: 9))
        XCTAssertEqual(state.selectedRange, NSRange(location: 3, length: 2))
        XCTAssertEqual(state.substring(in: NSRange(location: 2, length: 3)), "中😀")
        XCTAssertNil(state.substring(in: NSRange(location: 4, length: 1)))
        let edit = try XCTUnwrap(state.preedit("文字", selection: NSRange(location: 1, length: 1),
            replacing: NSRange(location: NSNotFound, length: 0)))
        XCTAssertEqual(edit.cursorBegin, 3)
        XCTAssertEqual(edit.cursorEnd, 6)
        XCTAssertEqual(state.markedRange, NSRange(location: 3, length: 2))
        XCTAssertEqual(state.selectedRange, NSRange(location: 4, length: 1))
        XCTAssertEqual(state.substring(in: NSRange(location: 2, length: 3)), "中文字")
    }

    func testReconversionDeletesBytesOnBothSidesAtomically() throws {
        var state = GuestTextInputState()
        XCTAssertTrue(state.updateSurrounding("ab中😀z", cursor: 5, anchor: 5))
        let edit = try XCTUnwrap(state.commit("文", replacing: NSRange(location: 2, length: 3)))
        XCTAssertEqual(edit.text, "文")
        XCTAssertEqual(edit.before, 3)
        XCTAssertEqual(edit.after, 4)
        XCTAssertEqual(state.surrounding, "ab文z")
        XCTAssertEqual(state.selectedRange, NSRange(location: 3, length: 0))
    }

    func testMarkedReplacementPreservesUnchangedComposition() throws {
        var state = GuestTextInputState()
        XCTAssertTrue(state.updateSurrounding("a中z", cursor: 1, anchor: 4))
        _ = try XCTUnwrap(state.preedit("甲乙丙", selection: NSRange(location: 3, length: 0),
            replacing: NSRange(location: NSNotFound, length: 0)))
        let edit = try XCTUnwrap(state.preedit("😀", selection: NSRange(location: 2, length: 0),
            replacing: NSRange(location: 2, length: 1)))
        XCTAssertEqual(edit.text, "甲😀丙")
        XCTAssertEqual(edit.cursorBegin, 7)
        XCTAssertEqual(state.selectedRange, NSRange(location: 4, length: 0))
        let commit = try XCTUnwrap(state.commit("丁", replacing: NSRange(location: 4, length: 1)))
        XCTAssertEqual(commit.text, "甲😀丁")
        XCTAssertEqual(state.surrounding, "a甲😀丁z")
    }

    func testInvalidRangesDoNotChangeTheSnapshot() throws {
        var state = GuestTextInputState()
        XCTAssertTrue(state.updateSurrounding("a😀z", cursor: 5, anchor: 5))
        XCTAssertFalse(state.updateSurrounding("a😀z", cursor: 2, anchor: 5))
        for range in [NSRange(location: 2, length: 1), NSRange(location: 0, length: 1),
                      NSRange(location: Int.max - 1, length: Int.max)] {
            XCTAssertNil(state.commit("中", replacing: range))
        }
        XCTAssertNil(state.preedit("😀", selection: NSRange(location: 1, length: 0),
            replacing: NSRange(location: NSNotFound, length: 0)))
        XCTAssertEqual(state.surrounding, "a😀z")
        XCTAssertTrue(state.markedText.isEmpty)
    }

    func testSensitiveFieldsDropContextAndRestrictRomanInput() throws {
        var state = GuestTextInputState()
        XCTAssertTrue(state.updateSurrounding("secret", cursor: 6, anchor: 6))
        state.setContentType(hints: 0, purpose: 8)
        XCTAssertTrue(state.requiresRomanInput)
        XCTAssertNil(state.surrounding)
        XCTAssertTrue(state.updateSurrounding("secret", cursor: 6, anchor: 6))
        XCTAssertNil(state.substring(in: NSRange(location: 0, length: 6)))
        XCTAssertNotNil(state.commit("a", replacing: NSRange(location: NSNotFound, length: 0)))
        state.setContentType(hints: 0, purpose: 0)
        XCTAssertFalse(state.requiresRomanInput)
        XCTAssertTrue(state.updateSurrounding("hello", cursor: 5, anchor: 5))
        XCTAssertEqual(state.surrounding, "hello")
        state.setContentType(hints: 0x80, purpose: 0)
        XCTAssertNil(state.surrounding)
        XCTAssertFalse(state.requiresRomanInput)
    }
}
