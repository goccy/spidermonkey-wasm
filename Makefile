# Bundle module path / go directive stamped into the wasm2go bundle's go.mod.
# The bundle is released as a self-contained Go module, so it needs a go.mod
# declaring the import path its own `import` / `//go:linkname` sites embed.
# wasmify writes that path into wasmify.json's bridge.Wasm2GoImportPath, which
# the codegen reads back; derive it from the JSON so the two never drift.
# The bundle is emitted at <bridge-dir>/internal/wasm2go. The bridge's Go
# package is github.com/goccy/go-spidermonkey/internal, so its dir is
# build/wasm2go/internal and the bundle nests one level deeper.
WASM2GO_BUNDLE_DIR    := build/wasm2go/internal/internal/wasm2go
WASM2GO_BUNDLE_GO_VER := 1.25.0

# The full pipeline, top to bottom. Unlike python-wasm's, this one runs on the
# host rather than inside ghcr.io/goccy/wasmify: the image carries no Rust
# toolchain, and rust/ has to be built (see `deps`). GitHub's ubuntu runners
# ship cargo, so CI needs nothing extra.
#
# There is no `wasmify build` / `generate-build` phase either. Those capture a
# native build and replay it under wasi-sdk; SpiderMonkey has no native build
# here — the engine arrives prebuilt (see scripts/fetch-spidermonkey.sh) — so
# build.json is a committed, stepless file and the archive reaches the link
# through wasmify.json's wasm_build.prebuilt_archives.
WASMIFY_PIPELINE = \
	make tools && \
	make deps && \
	wasmify parse-headers --header js.h && \
	wasmify gen-proto && \
	wasmify wasm-build --optimize --non-interactive && \
	rm -rf build/wasm2go && \
	buf generate --timeout 0 && \
	make bundle-gomod
# `rm -rf build/wasm2go` so `buf generate` writes the bundle into a CLEAN tree:
# protoc-gen-wasmify-go overwrites the files it emits but never deletes stale
# ones, and the bundle's file SET depends on the wasm's size (a sub-threshold
# wasm yields a single-package layout; a larger one yields base/ + pN). Mixing
# two leftover sets in one package does not compile.
#
# `--timeout 0` disables buf's default 2-minute command timeout. The plugin
# transpiles a 6.9k-function engine and, in its gcasm backend, compiles the
# emitted Go to capture assembly — well over two minutes of legitimate work.
# At the default buf SIGKILLs the plugin mid-run and reports only
# `signal: killed` (no OOM: measured cgroup peak stays under the limit and
# oom_kill stays 0). Zero means "no timeout", so the plugin runs to completion.

# The engine build runs in a Linux container; everything else runs on the host.
# CONTAINER is Apple's `container` on macOS; set it to `docker` (or `podman`) to
# use those instead — the flags below are common to all three.
CONTAINER       ?= container
ENGINE_IMAGE    ?= sm-engine-build
# A named volume for BUILD_ROOT. It holds the ~5 GB gecko checkout and the
# object dir, so keeping it turns a from-scratch engine build into an
# incremental one (minutes, not tens of minutes). It also has to be a volume
# rather than a bind mount from macOS: gecko does not build on a
# case-insensitive filesystem (it has both string.h and String.h), so the tree
# must live on the container's own filesystem.
ENGINE_VOLUME   ?= sm-engine-work
# The ThinLTO link of libjs_static peaks at several GB. The container default
# is 1 GB, which does NOT fail — it thrashes: measured 240 minutes of linker
# CPU and 5.4 TB of block I/O with no progress and no error, which reads
# exactly like a hang. Give it real memory, and enough cores for the parallel
# LTO backend.
ENGINE_MEMORY   ?= 12g
ENGINE_CPUS     ?= 8
# `release` is what ships; SPIDERMONKEY_DEBUG=1 selects the assertion build.
ENGINE_THREADS  ?= 1
ENGINE_FLAVOR   ?= release

.PHONY: all wasm wasm-clean tools deps engine engine-build engine-install engine-image bundle-gomod smoke help

# Install the tools wasmify.json declares (wasi-sdk, cargo). Safe to re-run;
# already-installed tools are skipped.
tools:
	wasmify ensure-tools . --output-dir .

# Build the SpiderMonkey engine itself — the ONLY step that needs a container,
# and the only way to change SpiderMonkey's own C++ (see the source patches in
# scripts/build-engine-intl.sh). `make wasm` does NOT need this: it links the
# prebuilt archive that already sits in deps/, so touching js.cc alone never
# requires an engine build.
#
# Why a container. gecko's mach supports only Linux and Windows hosts for a
# wasi cross-build. On macOS it fails during host detection, before configure
# gets anywhere near the target: it cannot parse any SDKSettings.plist (its
# Python needs a working pyexpat, which a Homebrew python routinely does not
# have), and it rejects Apple's linker because `ld -Wl,--version` is an error
# rather than a version string.
#
# Output lands in build/ ON THE HOST, because the repo is bind-mounted at /src,
# and is then INSTALLED into deps/ — see engine-install for why that step is not
# just `make deps`.
engine: engine-image
	@$(MAKE) engine-build
	@$(MAKE) engine-install

engine-build: engine-image
	$(CONTAINER) run --rm \
		-m $(ENGINE_MEMORY) -c $(ENGINE_CPUS) \
		-v "$(CURDIR)":/src \
		--mount type=volume,source=$(ENGINE_VOLUME),target=/work \
		-e BUILD_ROOT=/work \
		-e SPIDERMONKEY_THREADS=$(ENGINE_THREADS) \
		$(if $(SPIDERMONKEY_DEBUG),-e SPIDERMONKEY_DEBUG=$(SPIDERMONKEY_DEBUG),) \
		-w /src $(ENGINE_IMAGE) bash scripts/build-engine-intl.sh

# Install the archive `engine` just built into deps/, where the wasm link picks
# it up. This is NOT `make deps`: fetch-spidermonkey.sh short-circuits on a
# .tag stamp that only records the SpiderMonkey TAG, so a freshly rebuilt
# archive at the same tag is silently skipped — the build reports success and
# the old engine stays installed. That is not hypothetical: two engine builds
# in a row were measured against the previous binary before it was noticed.
# Dropping the stamp is what makes the fetch actually re-extract.
engine-install:
	rm -f deps/spidermonkey/.tag
	SPIDERMONKEY_LOCAL_ARCHIVE=build/spidermonkey-static-intl-$(ENGINE_FLAVOR).tar.gz \
		bash scripts/fetch-spidermonkey.sh
	@echo "==> installed $$(ls -la deps/spidermonkey/libspidermonkey.a | awk '{print $$6,$$7,$$8}')"

# Build the engine build image if it is not present. Cheap to re-run: the
# check is a listing, not a rebuild.
engine-image:
	@if ! $(CONTAINER) image list 2>/dev/null | awk '{print $$1}' | grep -qx '$(ENGINE_IMAGE)'; then \
		echo "==> building $(ENGINE_IMAGE) from Dockerfile.engine"; \
		$(CONTAINER) build -t $(ENGINE_IMAGE) -f Dockerfile.engine .; \
	else \
		echo "==> $(ENGINE_IMAGE) present"; \
	fi

# Materialise the two link inputs, neither of which is committed:
#   deps/spidermonkey/   the prebuilt engine archive + headers (~520 MB)
#   rust/target/         the Rust staticlib holding SpiderMonkey's string encoders
deps:
	bash scripts/fetch-spidermonkey.sh
	bash scripts/build-rust-crates.sh

# Build spidermonkey.wasm + the wasm2go bundle from a clean checkout, exactly the
# way .github/workflows/release.yml does. Outputs:
#   .wasmify/wasm-build/output/spidermonkey.wasm
#   build/spidermonkey.wasm                       <- wasmify.json's output.wasm
#   build/wasm2go/                                <- wasm2go bridge + bundle
#   build/wasm2go/internal/wasm2go/go.mod         <- bundle module manifest
#
# The link is a ThinLTO pass over the whole engine — libspidermonkey.a holds LLVM
# bitcode, not objects — so budget about a minute and several GB of RSS.
wasm:
	$(WASMIFY_PIPELINE)

# Write go.mod into the wasm2go bundle so the released tarball is a
# self-contained Go module. Parses bridge.Wasm2GoImportPath out of wasmify.json
# with grep+sed (a literal manifest; no jq, no Go toolchain needed).
bundle-gomod:
	@if [ ! -d "$(WASM2GO_BUNDLE_DIR)" ]; then \
		echo "$(WASM2GO_BUNDLE_DIR) does not exist — run 'make wasm' first" >&2; \
		exit 1; \
	fi
	@path=$$(grep -E '"Wasm2GoImportPath"[[:space:]]*:' wasmify.json \
		| head -1 \
		| sed -E 's/.*"Wasm2GoImportPath"[[:space:]]*:[[:space:]]*"([^"]+)".*/\1/'); \
	if [ -z "$$path" ]; then \
		echo "wasmify.json bridge.Wasm2GoImportPath is empty; cannot stamp bundle go.mod" >&2; \
		exit 1; \
	fi; \
	printf 'module %s\n\ngo %s\n' "$$path" "$(WASM2GO_BUNDLE_GO_VER)" \
		> $(WASM2GO_BUNDLE_DIR)/go.mod; \
	echo "wrote $(WASM2GO_BUNDLE_DIR)/go.mod (module $$path)"

# Exercise js.cc against the real engine under wasmtime, with no wasmify, no
# wasm2go and no Go in the picture. Covers the interrupt mechanism in both of its
# modes. The fastest way to tell whether a SpiderMonkey bump broke the engine
# layer or the Go layer.
smoke: deps
	bash scripts/run-smoke.sh

# Drop everything the pipeline regenerates so the next `make wasm` runs from
# scratch. The committed inputs (wasmify.json, build.json, buf.{yaml,gen.yaml},
# proto/wasmify, js.cc, js.h, rust/*.rs, rust/Cargo.*, scripts/, tests/, the
# starlingmonkey submodule) survive. deps/ and rust/target survive too: they are
# a pure function of the submodule pin and cost ~520 MB to refetch.
wasm-clean:
	rm -rf .wasmify api-spec.json proto/spidermonkey.proto bridge build

all: wasm

help:
	@echo 'Targets:'
	@echo '  wasm         Fetch deps, link spidermonkey.wasm, emit the wasm2go bundle'
	@echo '  deps         Fetch the prebuilt engine + build the Rust staticlib'
	@echo '  engine       Build SpiderMonkey itself in a container (only needed'
	@echo '               to change SpiderMonkey C++; make wasm does not use it)'
	@echo '  tools        Install wasi-sdk and the tools wasmify.json declares'
	@echo '  smoke        Run tests/smoke.cc under wasmtime, in both interrupt modes'
	@echo '  bundle-gomod Stamp go.mod into the wasm2go bundle'
	@echo '  wasm-clean   Drop generated artefacts; keep committed inputs and deps/'
