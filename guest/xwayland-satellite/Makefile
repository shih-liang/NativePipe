ARCH            ?= $(shell uname -m | sed 's/^arm64$$/aarch64/')
LIBC            ?= $(shell (ldd --version 2>&1 || true) | grep -qi musl && echo musl || echo gnu)
VERSION         := $(shell tr -d '[:space:]' < VERSION)
UPSTREAM_COMMIT := $(shell tr -d '[:space:]' < UPSTREAM_COMMIT)
UPSTREAM_URL    := https://github.com/Supreeeme/xwayland-satellite.git
SOURCE_DIR      := .build/source
PATCH           := patches/0001-drop-leave-without-forwarded-enter.patch
BINARY          := dist/xwayland-satellite-$(ARCH)-$(LIBC)
CARGO           ?= cargo
RUSTC           ?= rustc
STRIP           ?= strip
RUST_VERSION    ?= 1.89.0
LICENSE_NOTICE  := dist/xwayland-satellite-$(ARCH)-$(LIBC).third-party-licenses.txt
LICENSE_TOOL    := ../../scripts/generate-xwayland-license-notice.py

.PHONY: all dist-target check-arch verify-source test test-integration clean

all: dist-target
dist-target: check-arch $(BINARY) $(LICENSE_NOTICE)

check-arch:
	@actual="$$(uname -m | sed 's/^arm64$$/aarch64/')"; \
	[ "$$actual" = "$(ARCH)" ] || { \
		echo "error: native build runner is $$actual, requested $(ARCH)" >&2; exit 1; \
	}

$(SOURCE_DIR)/.patched: VERSION UPSTREAM_COMMIT $(PATCH)
	rm -rf $(SOURCE_DIR)
	mkdir -p $(SOURCE_DIR)
	git -C $(SOURCE_DIR) init -q
	git -C $(SOURCE_DIR) fetch -q --depth=1 $(UPSTREAM_URL) $(UPSTREAM_COMMIT)
	git -C $(SOURCE_DIR) checkout -q --detach FETCH_HEAD
	@test "$$(git -C $(SOURCE_DIR) rev-parse HEAD)" = "$(UPSTREAM_COMMIT)"
	git -C $(SOURCE_DIR) apply --check $(abspath $(PATCH))
	git -C $(SOURCE_DIR) apply $(abspath $(PATCH))
	@touch $@

verify-source: $(SOURCE_DIR)/.patched
	@test "$$(git -C $(SOURCE_DIR) rev-parse HEAD)" = "$(UPSTREAM_COMMIT)"
	@test "$$(tr -d '[:space:]' < VERSION)" = "$(VERSION)"

$(BINARY): $(SOURCE_DIR)/.patched
	cd $(SOURCE_DIR) && CARGO_INCREMENTAL=0 \
		RUSTFLAGS="--remap-path-prefix=$$(pwd)=/usr/src/xwayland-satellite -C link-arg=-Wl,--build-id=none" \
		$(CARGO) build --release --locked
	mkdir -p dist
	install -m0755 $(SOURCE_DIR)/target/release/xwayland-satellite $@
	$(STRIP) $@

$(LICENSE_NOTICE): $(BINARY) $(LICENSE_TOOL)
	python3 $(LICENSE_TOOL) \
		--source-dir $(SOURCE_DIR) --output $@ \
		--expected-rust-version $(RUST_VERSION) \
		--cargo $(CARGO) --rustc $(RUSTC)

test: $(SOURCE_DIR)/.patched
	cd $(SOURCE_DIR) && CARGO_INCREMENTAL=0 \
		RUSTFLAGS="--remap-path-prefix=$$(pwd)=/usr/src/xwayland-satellite" \
		$(CARGO) test --release --locked --lib

# The upstream integration suite starts a real Xwayland server and therefore
# needs an isolated graphical test host.  Keep it explicit: generic CI workers
# can have an existing X socket and produce the upstream "no click serial"
# failures even without the RemotePipe patch.
test-integration: $(SOURCE_DIR)/.patched
	cd $(SOURCE_DIR) && CARGO_INCREMENTAL=0 \
		RUSTFLAGS="--remap-path-prefix=$$(pwd)=/usr/src/xwayland-satellite" \
		$(CARGO) test --release --locked --test integration -- --test-threads=1

clean:
	rm -rf .build dist
