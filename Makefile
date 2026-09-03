ARTIFACTS_ROOT ?= $(abspath ../artifacts)
RELEASE_TAG ?=
DIST_DIR ?= $(CURDIR)/dist
SWIFT_FLAGS ?= --disable-sandbox --build-system native

.PHONY: test test-gpu nativepipe stage-release

test:
	NATIVEPIPE_OMIT_GPU=1 swift test $(SWIFT_FLAGS)

# Requires NATIVEPIPE_VIRGL_PREFIX, NATIVEPIPE_MOLTENVK_PREFIX and
# NATIVEPIPE_ANGLE_PREFIX, or equivalent SDKs under vendor/*-prefix.
test-gpu:
	swift test $(SWIFT_FLAGS)

nativepipe:
	swift build $(SWIFT_FLAGS) -c release --product nativepipe

# Release assets are built by this repository's CI. This target only stages an
# already-built, signed release for a FluxWindow application build.
stage-release:
	@test -n "$(RELEASE_TAG)" || { echo 'RELEASE_TAG is required' >&2; exit 1; }
	./scripts/stage-release-assets.sh "$(DIST_DIR)" "$(ARTIFACTS_ROOT)" \
		shih-liang/nativepipe "$(RELEASE_TAG)" nativepipe-runtime
