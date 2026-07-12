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

# --- source patches ------------------------------------------------------------
# On __wasi__, SpiderMonkey bounds recursion with a DEPTH COUNTER, not
# stack-pointer checks: AutoCheckRecursionLimit increments
# RootingContext::wasiRecursionDepth and CheckWasiRecursionLimit compares it
# against wasiRecursionDepthLimit — a `static constexpr 350`, tuned for the
# 1 MiB stack the upstream shell links with (js/src/shell/moz.build). It
# silently ignores JS_SetNativeStackQuota, so an embedding linking an 8 MiB
# stack (this project) is stuck at a ceiling its stack could carry 8x over.
#
# Make the limit a mutable per-context field with the same default, and
# advertise it with JS_HAS_MUTABLE_WASI_RECURSION_LIMIT so js.cc can scale it
# from js_new's native_stack_quota_bytes. Applied as verified targeted edits
# rather than a context diff, so an engine-tag bump that moves the code fails
# the build loudly instead of shipping an unpatched engine.
f=$SRC/js/public/RootingAPI.h
if ! grep -q 'JS_HAS_MUTABLE_WASI_RECURSION_LIMIT' "$f"; then
    perl -0pi -e 's/static constexpr uint32_t wasiRecursionDepthLimit = 350u;/uint32_t wasiRecursionDepthLimit = 350u;\n#  define JS_HAS_MUTABLE_WASI_RECURSION_LIMIT 1/' "$f"
fi
grep -q 'JS_HAS_MUTABLE_WASI_RECURSION_LIMIT' "$f" || {
    echo "error: wasi recursion-limit patch no longer applies to $f" >&2
    exit 1
}
f=$SRC/js/src/vm/JSContext.cpp
perl -0pi -e 's/JS::RootingContext::wasiRecursionDepthLimit\b/JS::RootingContext::get(cx)->wasiRecursionDepthLimit/g' "$f"
grep -q 'get(cx)->wasiRecursionDepthLimit' "$f" || {
    echo "error: wasi recursion-limit patch no longer applies to $f" >&2
    exit 1
}

# gecko's ThinLTO setup adds ELF-linker plugin options that wasm-ld rejects
# outright (-plugin-opt=-import-instr-limit, -plugin-opt=new-pass-manager,
# -plugin-opt=-import-hot-multiplier). Only the js shell links in this build
# — and the shell has to build, it is what drags jsrust into the graph — so
# guard those flags off for WASI targets.
f=$SRC/build/moz.configure/lto-pgo.configure
perl -0pi -e 's/    elif c_compiler\.type == "clang":\n        ldflags\.append\("-Wl,-plugin-opt=-import-instr-limit=10"\)/    elif c_compiler.type == "clang" and target.os != "WASI":\n        ldflags.append("-Wl,-plugin-opt=-import-instr-limit=10")/' "$f"
grep -q 'c_compiler.type == "clang" and target.os != "WASI"' "$f" || {
    echo "error: wasm-ld plugin-opt patch (instr-limit) no longer applies to $f" >&2
    exit 1
}
perl -0pi -e 's/        else:\n            if c_compiler\.version < "13\.0\.0":\n                ldflags\.append\("-Wl,-plugin-opt=new-pass-manager"\)\n            ldflags\.append\("-Wl,-plugin-opt=-import-hot-multiplier=30"\)/        elif target.os != "WASI":\n            if c_compiler.version < "13.0.0":\n                ldflags.append("-Wl,-plugin-opt=new-pass-manager")\n            ldflags.append("-Wl,-plugin-opt=-import-hot-multiplier=30")/' "$f"
grep -q 'elif target.os != "WASI":' "$f" || {
    echo "error: wasm-ld plugin-opt patch (hot-multiplier) no longer applies to $f" >&2
    exit 1
}

# 64-bit Atomics: AtomicOperations-feeling-lucky-gcc.h gates
# HAS_64BIT_ATOMICS behind an architecture allowlist that predates wasm32,
# so Atomics on a BigInt64Array MOZ_CRASHes the whole instance — a one-line
# guest-JS DoS against the sandbox. With -mthread-model single the 64-bit
# __atomic builtins lower to plain i64 operations, which are trivially
# correct for a single agent, so declaring support is sound. (Upstreamable:
# the same holds for any wasm32 build.)
f=$SRC/js/src/jit/shared/AtomicOperations-feeling-lucky-gcc.h
if ! grep -q '__wasm32__' "$f"; then
    perl -0pi -e 's/#if defined\(__riscv\) && __riscv_xlen == 64\n#  define HAS_64BIT_ATOMICS\n#  define HAS_64BIT_LOCKFREE\n#endif/#if defined(__riscv) \&\& __riscv_xlen == 64\n#  define HAS_64BIT_ATOMICS\n#  define HAS_64BIT_LOCKFREE\n#endif\n\n#if defined(__wasm32__)\n#  define HAS_64BIT_ATOMICS\n#  define HAS_64BIT_LOCKFREE\n#endif/' "$f"
fi
grep -q '__wasm32__' "$f" || {
    echo "error: wasm32 64-bit-atomics patch no longer applies to $f" >&2
    exit 1
}

# --- mozconfig -----------------------------------------------------------------
# StarlingMonkey's release mozconfig, verbatim, with ONE change: no
# --without-intl-api line, so the build defaults to --with-intl-api and
# bundles ICU (data compiled in; wasi needs no filesystem for it).
MOZCONFIG=$OBJ-mozconfig
mkdir -p "$(dirname "$MOZCONFIG")"
# Also unlike StarlingMonkey's, shared memory stays ENABLED: a single agent
# with SharedArrayBuffer + non-blocking Atomics is spec-conformant without any
# threads (Atomics.wait must throw on an agent whose [[CanBlock]] is false),
# and it is the first stage toward goroutine-backed agents via wasi-threads.
# Unlike StarlingMonkey's mozconfig, the js shell stays ENABLED: js/src/rust
# (jsrust — encoding_rs, ICU4X capi, Temporal) is only in the build graph
# behind `if not CONFIG["JS_DISABLE_SHELL"]` (js/src/moz.build), and a
# with-intl engine cannot link without it. The shell binary itself is
# discarded; it just drags jsrust into the build.
# SPIDERMONKEY_THREADS=1 builds the engine for wasi-threads: real pthreads
# instead of -mthread-model single, so several JS agents (each its own
# JSContext) can run concurrently — which is what test262's $262.agent needs
# and what wasm2go's goroutine-backed wasi_thread_spawn can actually host.
# There is no --enable-threadsafe knob in modern SpiderMonkey (it was removed
# long ago; the engine is threadsafe by construction). What decides whether the
# wasi build gets real threads is the TOOLCHAIN: compiling against wasi-sdk's
# wasm32-wasi-threads sysroot with -pthread gives it pthreads, TLS and atomics,
# which is what js/src/threading/posix and mozglue's real (non-noop) mutexes
# need. So the threads build differs only in the flags mach_build passes.
THREADS_MOZOPTS=""
if [[ -n ${SPIDERMONKEY_THREADS:-} ]]; then
    echo "[engine] threads build (wasm32-wasi-threads sysroot, -pthread)"
fi

cat > "$MOZCONFIG" <<EOF
ac_add_options --enable-project=js
ac_add_options --target=wasm32-unknown-wasi
$THREADS_MOZOPTS
ac_add_options --without-system-zlib
ac_add_options --disable-jit
ac_add_options --disable-shared-js
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
mach_build() {
    env CC="$WASI_SDK_PATH/bin/clang" \
        CXX="$WASI_SDK_PATH/bin/clang++" \
        AR="$WASI_SDK_PATH/bin/llvm-ar" \
        HOST_CC="${HOST_CC:-clang}" \
        HOST_CXX="${HOST_CXX:-clang++}" \
        MOZCONFIG="$MOZCONFIG" \
        python3 "$SRC/mach" --no-interactive build "$@"
}
mach_build

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
    echo "[engine] jsrust candidates in the objdir:" >&2
    find "$OBJ" -iname '*jsrust*' >&2 || true
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
