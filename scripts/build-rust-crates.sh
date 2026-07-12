#!/usr/bin/env bash
#
# build-rust-crates.sh — build the Rust staticlib SpiderMonkey links against.
#
# This is spidermonkey-wasm's `build_commands.build`. libspidermonkey.a leaves
# SpiderMonkey's string-encoding routines undefined (they live in the encoding_rs
# crate, reached via encoding_c / encoding_c_mem) plus install_rust_hooks. rust/
# bundles both crates — taken from the pinned `starlingmonkey` submodule, so they
# match the engine they will be linked with — into one archive, which
# wasmify.json lists in wasm_build.prebuilt_archives.
#
# Toolchain is pinned to what StarlingMonkey pins
# (starlingmonkey/rust-toolchain.toml), because that is the combination its
# Cargo.lock was resolved against.
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$repo_root"

toolchain_file=starlingmonkey/rust-toolchain.toml
if [[ ! -f $toolchain_file ]]; then
    echo "error: $toolchain_file not found. Run: git submodule update --init starlingmonkey" >&2
    exit 1
fi
CHANNEL=$(sed -n 's/^channel *= *"\([^"]*\)".*/\1/p' "$toolchain_file")
: "${CHANNEL:?could not parse channel from $toolchain_file}"

# A threads build links against a SHARED memory, and wasm-ld rejects any object
# that lacks the atomics/bulk-memory features — including Rust's. The
# wasm32-wasip1-threads target ships a prebuilt std that has them.
if [[ -n ${SPIDERMONKEY_THREADS:-} ]]; then
    TARGET=wasm32-wasip1-threads
else
    TARGET=wasm32-wasip1
fi

if command -v rustup >/dev/null; then
    run_cargo() { rustup run "$CHANNEL" cargo "$@"; }
    if ! rustup run "$CHANNEL" rustc --version >/dev/null 2>&1; then
        echo "[build-rust-crates] installing Rust $CHANNEL ($TARGET)"
        rustup toolchain install "$CHANNEL" --profile minimal --target "$TARGET"
    fi
    # The toolchain may predate this TARGET (a threads build on a toolchain
    # installed for the plain one); target add is idempotent, so just ensure it.
    rustup target add --toolchain "$CHANNEL" "$TARGET"
else
    # No rustup: fall back to whatever cargo is on PATH and let it fail loudly if
    # the wasm target's std is missing.
    echo "[build-rust-crates] rustup not found; using 'cargo' from PATH" >&2
    run_cargo() { cargo "$@"; }
fi

echo "[build-rust-crates] cargo build --release --target $TARGET"
( cd rust && run_cargo build --release --locked --target "$TARGET" )

out=rust/target/$TARGET/release/libspidermonkey_rust.a
if [[ ! -f $out ]]; then
    echo "error: expected $out to exist after the build" >&2
    exit 1
fi
echo "[build-rust-crates] built $out ($(du -h "$out" | cut -f1))"
