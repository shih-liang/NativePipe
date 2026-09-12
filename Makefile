SWIFT_FLAGS ?= --disable-sandbox --build-system native

.PHONY: test nativepipe codecs

codecs:
	python3 scripts/build-codecs.py macos

test: codecs
	swift test $(SWIFT_FLAGS)

nativepipe: codecs
	swift build $(SWIFT_FLAGS) -c release --product nativepipe
