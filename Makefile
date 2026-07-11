# Bundle module path / go directive stamped into the wasm2go bundle's go.mod.
# The bundle is released as a self-contained Go module, so it needs a go.mod
# declaring the import path its own `import` / `//go:linkname` sites embed.
# wasmify writes that path into wasmify.json's bridge.Wasm2GoImportPath, which
# the codegen reads back; derive it from the JSON so the two never drift.
WASM2GO_BUNDLE_DIR    := build/wasm2go/internal/wasm2go
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

.PHONY: all wasm wasm-clean tools deps bundle-gomod smoke help

# Install the tools wasmify.json declares (wasi-sdk, cargo). Safe to re-run;
# already-installed tools are skipped.
tools:
	wasmify ensure-tools . --output-dir .

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
	@echo '  tools        Install wasi-sdk and the tools wasmify.json declares'
	@echo '  smoke        Run tests/smoke.cc under wasmtime, in both interrupt modes'
	@echo '  bundle-gomod Stamp go.mod into the wasm2go bundle'
	@echo '  wasm-clean   Drop generated artefacts; keep committed inputs and deps/'
