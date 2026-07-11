#!/usr/bin/env bash
#
# run-smoke.sh — build tests/smoke.cc into a WASI command and run it under
# wasmtime, once per interrupt mode.
#
# This exercises js.cc against the real engine without wasmify, wasm2go or Go in
# the picture, so a failure here localises to the C++ embedding layer. The Go
# bindings have their own tests; what they cannot cover from Go is nothing, and
# what this cannot cover is concurrency (a host thread interrupting a spinning
# guest) — the stores are performed from inside the guest here.
#
# Requires: wasi-sdk (WASI_SDK_PATH or wasmify's copy), wasmtime, and a
# completed `make deps`.
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$repo_root"

SDK=${WASI_SDK_PATH:-$HOME/.config/wasmify/bin/wasi-sdk}
if [[ ! -x $SDK/bin/clang++ ]]; then
    echo "error: wasi-sdk not found at $SDK (set WASI_SDK_PATH)" >&2
    exit 1
fi
WASMTIME=${WASMTIME:-wasmtime}
if ! command -v "$WASMTIME" >/dev/null; then
    echo "error: wasmtime not found (set WASMTIME)" >&2
    exit 1
fi

# SPIDERMONKEY_DIST overrides the engine under test — e.g. build/engine-pkg,
# the with-Intl archive scripts/build-engine-intl.sh produces.
SM=${SPIDERMONKEY_DIST:-deps/spidermonkey}
# Exactly one Rust staticlib links into the wasm (each carries the Rust
# runtime). With-intl engine dists ship mach's own jsrust (encoding_rs +
# ICU4X + Temporal); the StarlingMonkey prebuilt ships none, so the thin
# local staticlib (encoding_rs only) fills in.
if [[ -f $SM/libjsrust.a ]]; then
    RUSTLIB=$SM/libjsrust.a
else
    RUSTLIB=rust/target/wasm32-wasip1/release/libspidermonkey_rust.a
fi
for f in "$SM/libspidermonkey.a" "$RUSTLIB"; do
    if [[ ! -f $f ]]; then
        echo "error: $f missing. Run: make deps" >&2
        exit 1
    fi
done

out=${OUT_DIR:-.wasmify/smoke}
mkdir -p "$out"

# Flags mirror what wasmify.json feeds the real build: the bridge has to be
# compiled with the same ABI-affecting options as libspidermonkey.a.
CXXFLAGS=(
    --target=wasm32-wasip1 --sysroot="$SDK/share/wasi-sysroot"
    -std=gnu++20 -fno-rtti -fno-exceptions -fno-sized-deallocation
    -fno-aligned-new -mthread-model single -Wno-invalid-offsetof -DNDEBUG -O2
    # SpiderMonkey's own headers trip these under clang; they are diagnostics
    # about mozilla/ code, not about ours.
    -Wno-unknown-warning-option -Wno-unused-result
)

echo "[smoke] compiling"
"$SDK/bin/clang++" "${CXXFLAGS[@]}" -c -o "$out/js.o" js.cc -I "$SM/include"
"$SDK/bin/clang++" "${CXXFLAGS[@]}" -c -o "$out/wrappers.o" \
    starlingmonkey/crates/rust-hooks/src/wrappers.cpp \
    -I "$SM/include" -include "$SM/include/js-confdefs.h"
"$SDK/bin/clang++" "${CXXFLAGS[@]}" -c -o "$out/smoke.o" tests/smoke.cc -I .

echo "[smoke] linking (ThinLTO over libspidermonkey.a; expect ~30s and several GB of RSS)"
"$SDK/bin/clang++" \
    --target=wasm32-wasip1 --sysroot="$SDK/share/wasi-sysroot" -O2 \
    -o "$out/smoke.wasm" \
    "$out/smoke.o" "$out/js.o" "$out/wrappers.o" \
    "$SM/libspidermonkey.a" "$RUSTLIB" \
    -Wl,-z,stack-size=8388608 -Wl,--stack-first \
    -Wl,--max-memory=268435456 \
    -lwasi-emulated-signal -lwasi-emulated-process-clocks \
    -lwasi-emulated-getpid -lwasi-emulated-mman -lsetjmp

# --max-memory is what makes the heap-cap assertion meaningful: without a
# declared maximum the guest could grow linear memory until the host runs out.
# 256 MiB leaves the 64 MiB JS heap cap enough headroom to fail gracefully
# rather than trapping (see docs: the GC needs roughly 4x its cap in slack).

status=0
for mode in discovery fallback; do
    echo
    echo "[smoke] === $mode ==="
    # Built by growing a non-empty array: under `set -u`, bash 3.2 (what macOS
    # ships) treats "${empty[@]}" as an unbound variable and aborts.
    cmd=("$WASMTIME" run)
    if [[ $mode == fallback ]]; then
        # wasmtime does not forward the host environment to the guest, so the
        # variable has to be handed over explicitly.
        cmd+=(--env SPIDERMONKEY_WASM_NO_INTERRUPT_DISCOVERY=1)
    fi
    if [[ -n ${SPIDERMONKEY_WASM_EXPECT_INTL:-} ]]; then
        # Tells smoke.cc to assert that Intl IS present (with-Intl engine
        # builds); without it, smoke asserts Intl is absent.
        cmd+=(--env SPIDERMONKEY_WASM_EXPECT_INTL=1)
    fi
    cmd+=("$out/smoke.wasm")
    if ! "${cmd[@]}"; then
        status=1
    fi
done

exit $status
