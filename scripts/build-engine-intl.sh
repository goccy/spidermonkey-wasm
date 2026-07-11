#!/usr/bin/env bash
#
# build-engine-intl.sh — build libspidermonkey.a for wasm32-wasi FROM SOURCE,
# with the Intl API (ICU) ENABLED.
#
# The archive fetch-spidermonkey.sh downloads is StarlingMonkey's prebuilt,
# which is configured --without-intl-api: no Intl, no Temporal, no regexp
# \p{...} property escapes, no String.prototype.normalize, no full Unicode
# case folding. Upstream SpiderMonkey supports ICU + Intl on wasi (Bugzilla
# 1706949, RESOLVED FIXED; the tree carries intl/icu-patches/*wasi*), so the
# gap is purely the mozconfig flag. This script reproduces StarlingMonkey's
# engine build (see starlingmonkey/cmake/spidermonkey.cmake) minus that flag.
#
# Runs in CI (.github/workflows/engine.yml). A local run needs ~10 GB of
# disk, python3.11+, rust with the wasm32-wasip1 target, cbindgen, a host
# clang/clang++, and WASI_SDK_PATH pointing at wasi-sdk >= 30.
#
# Outputs:
#   build/engine-pkg/                                the dist layout the rest
#     libspidermonkey.a                              of this repo consumes
#     include/ (incl. js-confdefs.h)                 (same as deps/spidermonkey)
#   build/spidermonkey-static-intl-release.tar.gz    the same, one dir deep,
#                                                    fetch-spidermonkey.sh-shaped
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$repo_root"

: "${WASI_SDK_PATH:?set WASI_SDK_PATH to a wasi-sdk >= 30 install}"

# The engine pin: same single source of truth fetch-spidermonkey.sh parses.
sm_cmake=starlingmonkey/cmake/spidermonkey.cmake
SM_TAG=$(sed -n 's/^set(SM_TAG \([A-Za-z0-9_]*\))$/\1/p' "$sm_cmake")
if [[ -z ${SM_TAG:-} ]]; then
    echo "error: could not parse SM_TAG from $sm_cmake" >&2
    exit 1
fi
echo "[engine] SM_TAG=$SM_TAG"

SRC=$repo_root/build/engine-src
OBJ=$repo_root/build/engine-obj
PKG=$repo_root/build/engine-pkg

# --- source ------------------------------------------------------------------
# bytecodealliance/firefox is the fork StarlingMonkey builds from; the tag
# carries their wasi fixes for exactly this engine version.
if [[ ! -d $SRC/.git ]]; then
    git clone --depth 1 --branch "$SM_TAG" \
        https://github.com/bytecodealliance/firefox.git "$SRC"
fi

# --- mozconfig -----------------------------------------------------------------
# StarlingMonkey's release mozconfig, verbatim, with ONE change: no
# --without-intl-api line, so the build defaults to --with-intl-api and
# bundles ICU (data compiled in; wasi needs no filesystem for it).
MOZCONFIG=$OBJ-mozconfig
mkdir -p "$(dirname "$MOZCONFIG")"
cat > "$MOZCONFIG" <<EOF
ac_add_options --enable-project=js
ac_add_options --disable-js-shell
ac_add_options --target=wasm32-unknown-wasi
ac_add_options --without-system-zlib
ac_add_options --disable-jit
ac_add_options --disable-shared-js
ac_add_options --disable-shared-memory
ac_add_options --disable-tests
ac_add_options --disable-clang-plugin
ac_add_options --enable-jitspew
ac_add_options --enable-optimize=-O3
ac_add_options --enable-js-streams
ac_add_options --enable-portable-baseline-interp
ac_add_options --prefix=$OBJ/dist
ac_add_options --with-sysroot=$WASI_SDK_PATH/share/wasi-sysroot
ac_add_options --disable-debug
ac_add_options --enable-lto=thin
mk_add_options MOZ_OBJDIR=$OBJ
mk_add_options AUTOCLOBBER=1
EOF
case "$(uname -s)" in
Linux)  echo "ac_add_options --disable-stdcxx-compat" >> "$MOZCONFIG" ;;
Darwin) echo "ac_add_options --host=$(uname -m | sed 's/arm64/aarch64/')-apple-darwin" >> "$MOZCONFIG" ;;
*)      echo "error: unsupported host $(uname -s)" >&2; exit 1 ;;
esac
echo "[engine] mozconfig:"
cat "$MOZCONFIG"

# --- build ---------------------------------------------------------------------
# CC/CXX/AR target wasm32-wasi (the wasi-sdk toolchain); HOST_CC/HOST_CXX build
# mach's host tools. Same split starlingmonkey/cmake/spidermonkey.cmake uses.
env CC="$WASI_SDK_PATH/bin/clang" \
    CXX="$WASI_SDK_PATH/bin/clang++" \
    AR="$WASI_SDK_PATH/bin/llvm-ar" \
    HOST_CC="${HOST_CC:-clang}" \
    HOST_CXX="${HOST_CXX:-clang++}" \
    MOZCONFIG="$MOZCONFIG" \
    python3 "$SRC/mach" --no-interactive build

# --- package -------------------------------------------------------------------
# libspidermonkey.a = libjs_static.a + the mozglue/mfbt/memory objects that are
# needed for linking but live outside it. The object list is StarlingMonkey's
# (SM_OBJ_FILES in cmake/spidermonkey.cmake).
SM_OBJ_FILES=(
    memory/build/Unified_cpp_memory_build0.o
    memory/mozalloc/Unified_cpp_memory_mozalloc0.o
    mfbt/Unified_cpp_mfbt0.o
    mfbt/Unified_cpp_mfbt1.o
    mozglue/misc/AutoProfilerLabel.o
    mozglue/misc/ConditionVariable_noop.o
    mozglue/misc/Debug.o
    mozglue/misc/Decimal.o
    mozglue/misc/MmapFaultHandler.o
    mozglue/misc/Mutex_noop.o
    mozglue/misc/Now.o
    mozglue/misc/Printf.o
    mozglue/misc/SIMD.o
    mozglue/misc/StackWalk.o
    mozglue/misc/TimeStamp.o
    mozglue/misc/TimeStamp_posix.o
    mozglue/misc/Uptime.o
    mozglue/static/lz4.o
    mozglue/static/lz4frame.o
    mozglue/static/lz4hc.o
    mozglue/static/xxhash.o
    third_party/fmt/Unified_cpp_third_party_fmt0.o
)

rm -rf "$PKG"
mkdir -p "$PKG"
# -L: dist/include is a tree of symlinks into the objdir; the archive needs
# the real files.
cp -RL "$OBJ/dist/include" "$PKG/include"
# The generated config headers live in the objdir, not dist/include; every
# consumer includes them (js-config.h via jstypes.h, js-confdefs.h via
# -include), so they travel with the archive.
for hdr in js-confdefs.h js-config.h; do
    if [[ ! -f $PKG/include/$hdr ]]; then
        cp "$OBJ/js/src/$hdr" "$PKG/include/$hdr"
    fi
done

cp "$OBJ/js/src/build/libjs_static.a" "$PKG/libspidermonkey.a"
missing=0
for obj in "${SM_OBJ_FILES[@]}"; do
    if [[ ! -f $OBJ/$obj ]]; then
        echo "[engine] MISSING expected object: $obj" >&2
        missing=1
        continue
    fi
    "$WASI_SDK_PATH/bin/llvm-ar" -q "$PKG/libspidermonkey.a" "$OBJ/$obj"
done
if [[ $missing -ne 0 ]]; then
    echo "error: the StarlingMonkey object list no longer matches this build" >&2
    exit 1
fi

# Introspection for the ICU pieces: with-intl builds put ICU (and its data)
# under config/external/icu. If any of it is NOT already inside libjs_static.a,
# append it — and say so, loudly, so packaging drift is visible in the CI log.
echo "[engine] icu members already in libjs_static.a:"
"$WASI_SDK_PATH/bin/llvm-ar" t "$OBJ/js/src/build/libjs_static.a" | grep -ci icu || true
if [[ -d $OBJ/config/external/icu ]]; then
    echo "[engine] objects under config/external/icu:"
    find "$OBJ/config/external/icu" -name '*.o' | sed "s|$OBJ/||"
    inlib=$("$WASI_SDK_PATH/bin/llvm-ar" t "$OBJ/js/src/build/libjs_static.a")
    while IFS= read -r o; do
        base=$(basename "$o")
        if ! grep -qx "$base" <<< "$inlib"; then
            echo "[engine] appending $o (not in libjs_static.a)"
            "$WASI_SDK_PATH/bin/llvm-ar" -q "$PKG/libspidermonkey.a" "$o"
        fi
    done < <(find "$OBJ/config/external/icu" -name '*.o')
fi

# jsrust: the Rust side of the engine (encoding_rs, and — with Intl on —
# ICU4X capi and Temporal). Without-intl builds barely use it, which is why
# the StarlingMonkey flow substitutes its own thin staticlib (rust/); a
# with-intl engine references icu4x_* symbols from C++ and must link the
# jsrust mach actually built. Ship it alongside; exactly one Rust staticlib
# may be linked into the final wasm (each carries the Rust runtime), so
# consumers use THIS ONE INSTEAD OF rust/'s (see run-smoke.sh).
JSRUST=$(find "$OBJ" -name 'libjsrust.a' | head -1)
if [[ -z $JSRUST ]]; then
    echo "error: libjsrust.a not found in the objdir" >&2
    exit 1
fi
echo "[engine] jsrust: $JSRUST"
echo "[engine] jsrust icu4x members: $("$WASI_SDK_PATH/bin/llvm-ar" t "$JSRUST" | grep -ci icu || true)"
echo "[engine] jsrust encoding members: $("$WASI_SDK_PATH/bin/llvm-ar" t "$JSRUST" | grep -ci encoding || true)"
cp "$JSRUST" "$PKG/libjsrust.a"

# fetch-spidermonkey.sh-shaped tarball: one top-level dir, stripped on unpack.
tar -czf build/spidermonkey-static-intl-release.tar.gz \
    -C "$PKG/.." "$(basename "$PKG")" \
    --transform "s|^$(basename "$PKG")|spidermonkey-dist-intl-release|"
echo "[engine] wrote build/spidermonkey-static-intl-release.tar.gz"
ls -lh build/spidermonkey-static-intl-release.tar.gz "$PKG/libspidermonkey.a"
