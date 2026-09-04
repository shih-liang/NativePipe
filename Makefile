SWIFT_FLAGS ?= --disable-sandbox --build-system native

.PHONY: test nativepipe

test:
	swift test $(SWIFT_FLAGS)

nativepipe:
	swift build $(SWIFT_FLAGS) -c release --product nativepipe
