import Foundation

/// An agent request to the compositor's AT-SPI2 worker, for one toplevel
/// window. Element references are opaque strings from the same window's tree.
public struct GuestAccessibilityRequest: Sendable, Equatable {
    public enum Operation: UInt32, Sendable { case tree = 1, action, focus, setText, setValue }
    public static let maximumElementSize = 1024
    public static let maximumArgumentSize = 65_536
    public static let maximumReplySize = 1024 * 1024

    public var window: UInt32
    public var operation: Operation
    /// Tree depth, action index, or new value, depending on the operation.
    public var number: Double
    public var element: String
    /// Action name or replacement text.
    public var argument: String
    public var includeHidden: Bool
    /// Up to 64K characters of an element's text instead of 1K.
    public var fullText: Bool

    public init(window: UInt32, operation: Operation, number: Double = 0, element: String = "",
                argument: String = "", includeHidden: Bool = false, fullText: Bool = false) {
        self.window = window; self.operation = operation; self.number = number
        self.element = element; self.argument = argument
        self.includeHidden = includeHidden; self.fullText = fullText
    }
    var flags: UInt32 { (includeHidden ? 1 : 0) | (fullText ? 2 : 0) }
}

/// One accessible object. Bounds are top-left window content points, the same
/// space as computer input, as `[x, y, width, height]`.
public struct GuestAccessibilityNode: Codable, Sendable, Equatable {
    public var ref: String
    public var role: String
    public var name: String?
    public var description: String?
    public var states: [String]
    public var bounds: [Double]?
    public var actions: [String]?
    public var text: String?
    /// The full length when `text` is a prefix.
    public var textLength: Int?
    public var value: Double?
    public var minimum: Double?
    public var maximum: Double?
    public var children: [GuestAccessibilityNode]?
    /// Present when children were left out: below the requested depth, or
    /// beyond the per-node limit.
    public var childCount: Int?

    public init(ref: String, role: String, name: String? = nil, states: [String] = [],
                bounds: [Double]? = nil, actions: [String]? = nil, children: [GuestAccessibilityNode]? = nil) {
        self.ref = ref; self.role = role; self.name = name; self.states = states
        self.bounds = bounds; self.actions = actions; self.children = children
    }
}

public struct GuestAccessibilityTree: Codable, Sendable, Equatable {
    public var application: String
    public var toolkit: String
    public var pid: Int32
    /// The window's own element reference.
    public var window: String
    public var root: GuestAccessibilityNode?
    /// The node, size or time budget ran out; narrow the request.
    public var truncated: Bool

    public init(application: String, toolkit: String, pid: Int32, window: String,
                root: GuestAccessibilityNode?, truncated: Bool) {
        self.application = application; self.toolkit = toolkit; self.pid = pid
        self.window = window; self.root = root; self.truncated = truncated
    }
}
