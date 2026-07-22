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

# BUILD_ROOT relocates the (huge, case-sensitive) build tree. gecko cannot be
# built on a case-insensitive filesystem — it has both string.h and String.h —
# so a container build on a macOS bind mount must put the tree on the
# container's own filesystem and copy only the finished archive back.
build_root=${BUILD_ROOT:-$repo_root/build}
mkdir -p "$build_root"
SRC=$build_root/engine-src
OBJ=$build_root/engine-obj
PKG=$build_root/engine-pkg

# --- source ------------------------------------------------------------------
# bytecodealliance/firefox is the fork StarlingMonkey builds from; the tag
# carries their wasi fixes for exactly this engine version.
# Reuse an existing checkout when it is already at the right tag: the tree is
# ~5 GB and mach builds incrementally, so a cached BUILD_ROOT (a container
# volume, say) turns a 15-minute rebuild into a 2-minute one. A checkout at a
# DIFFERENT tag is refetched; a dirty one is reset, because the patches below
# are applied in place and re-applying them to an already-patched tree would
# fail its own verification.
if [[ -d $SRC/.git ]]; then
    have=$(git -C "$SRC" describe --tags --exact-match 2>/dev/null || git -C "$SRC" rev-parse --abbrev-ref HEAD 2>/dev/null || echo unknown)
    if [[ $have == "$SM_TAG" ]]; then
        echo "[engine] reusing cached checkout at $SM_TAG"
        git -C "$SRC" checkout -- . 2>/dev/null || true
        git -C "$SRC" clean -qfd 2>/dev/null || true
    else
        echo "[engine] cached checkout is at '$have', want $SM_TAG — refetching"
        rm -rf "$SRC"
    fi
fi
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

# Portable baseline interpreter: upstream compiles its interrupt checks OUT
# on __wasi__ ("with a single thread, there is no possibility for an
# interrupt to come asynchronously") — but this embedding delivers
# interrupts exactly that way: the HOST stores JSContext::interruptBits_
# into linear memory while the guest runs (js.cc discover_interrupt_bits /
# the armed fallback), from a Go goroutine under wasm2go and from another
# thread under wasmtime. Without the checks a runaway script under PBL is
# unstoppable: js_eval cancellation and js_close both hang. Keep the checks
# on wasi; the cost is the same per-loop-head flag load js::Interpret pays.
f=$SRC/js/src/vm/PortableBaselineInterpret.cpp
if ! grep -q 'interrupt checks stay ON for wasi' "$f"; then
    perl -0pi -e 's|// Whether to compile in interrupt checks in the main interpreter loop\.\n#ifndef __wasi__\n// On WASI, with a single thread, there is no possibility for an\n// interrupt to come asynchronously\.\n#  define ENABLE_INTERRUPT_CHECKS\n#endif|// Whether to compile in interrupt checks in the main interpreter loop.\n// [spidermonkey-wasm] interrupt checks stay ON for wasi: the embedding\n// stores JSContext::interruptBits_ into linear memory from the host while\n// the guest runs, so asynchronous interrupts DO happen here.\n#define ENABLE_INTERRUPT_CHECKS|' "$f"
fi
grep -q 'interrupt checks stay ON for wasi' "$f" || {
    echo "error: wasi PBL interrupt-checks patch no longer applies to $f" >&2
    exit 1
}


# --- threads patches (SPIDERMONKEY_THREADS=1) --------------------------------
# WASI's threading in gecko is hard-wired OFF: js/src/moz.build picks
# threading/noop/NoopThread.cpp and mozglue/misc/moz.build picks
# Mutex_noop/ConditionVariable_noop for OS_ARCH == WASI ("WASI hasn't supported
# thread yet"). Neither -pthread nor the wasm32-wasi-threads sysroot changes
# that — the first threads build produced a js-confdefs.h byte-identical to the
# single-agent one. Real agents need the POSIX implementations, which work
# because wasi-libc's threads build provides pthreads on top of
# wasi_thread_spawn (a goroutine, under wasm2go).
if [[ -n ${SPIDERMONKEY_THREADS:-} ]]; then
    f=$SRC/js/src/moz.build
    perl -0pi -e 's{# WASI hasn.t supported thread yet so noop implementation is used\.\nelif CONFIG\["OS_ARCH"\] == "WASI":\n    UNIFIED_SOURCES \+= \[\n        "threading/noop/CpuCount\.cpp",\n        "threading/noop/NoopThread\.cpp",\n    \]\n}{# wasi-threads: pthreads come from wasi-libc, backed by wasi_thread_spawn.\nelif CONFIG["OS_ARCH"] == "WASI":\n    UNIFIED_SOURCES += [\n        "threading/posix/CpuCount.cpp",\n        "threading/posix/PosixThread.cpp",\n    ]\n}' "$f"
    grep -q 'threading/posix/PosixThread.cpp' "$f" || {
        echo "error: WASI threading patch no longer applies to $f" >&2
        exit 1
    }

    # PlatformMutex.h / PlatformConditionVariable.h reserve a fixed dummy
    # buffer for __wasi__ instead of sizing it from pthread_mutex_t /
    # pthread_cond_t (they assume WASI has no pthreads). With the real posix
    # implementations that buffer is too small — the static_assert fires. Let
    # wasi size it from the pthread types like every other posix platform.
    for f in $SRC/mozglue/misc/PlatformMutex.h $SRC/mozglue/misc/PlatformConditionVariable.h; do
        # /g: each header carries the guard TWICE (the pthread.h include and the
        # platformData_ sizing). Patching only the first left the dummy buffer in
        # place and the static_assert still fired.
        perl -0pi -e 's/#if !defined\(XP_WIN\) && !defined\(__wasi__\)/#if !defined(XP_WIN)/g' "$f"
        if grep -q '__wasi__' "$f"; then
            echo "error: wasi platform-data patch left a __wasi__ guard in $f" >&2
            exit 1
        fi
    done

    # ThisThread::SetName's fallback branch calls pthread_setname_np
    # unconditionally, but wasi-libc has no such function (configure detects
    # that and defines neither HAVE_PTHREAD_SETNAME_NP nor its variants — yet
    # the #else still calls it, so the link fails on an undefined symbol).
    # Thread names are debug ergonomics only; make it a no-op on wasi.
    f=$SRC/js/src/threading/posix/PosixThread.cpp
    perl -0pi -e 's/  int rv;\n#ifdef XP_DARWIN/  int rv;\n#if defined(__wasi__)\n  \/\* wasi-libc has no pthread_setname_np; thread names are debug-only. *\/\n  (void)name;\n  rv = 0;\n#elif defined(XP_DARWIN)/' "$f"
    grep -q '#if defined(__wasi__)' "$f" || {
        echo "error: wasi setname patch no longer applies to $f" >&2
        exit 1
    }

    # The Rust side must carry the atomics/bulk-memory features too, or wasm-ld
    # refuses --shared-memory ("not compiled with 'atomics' or 'bulk-memory'").
    # Rust ships a wasm32-wasip1-threads target whose PREBUILT std has them;
    # gecko derives the rust target from the C triple and has no override, so
    # teach rust.configure to honour one.
    f=$SRC/build/moz.configure/rust.configure
    if ! grep -q 'MOZ_RUST_TARGET_OVERRIDE' "$f"; then
        # The configure sandbox forbids bare `import`; @imports is its blessed
        # mechanism for pulling in environ.
        perl -0pi -e 's/\@checking\("for rust target triplet"\)\ndef rust_target_triple\(/\@checking("for rust target triplet")\n\@imports(_from="os", _import="environ")\ndef rust_target_triple(/' "$f"
        perl -0pi -e 's/    rustc_target = detect_rustc_target\(\n        target, compiler_info, arm_target, rust_supported_targets\n    \)/    _override = environ.get("MOZ_RUST_TARGET_OVERRIDE")\n    if _override:\n        assert_rust_compile(target, _override, rustc)\n        return _override\n    rustc_target = detect_rustc_target(\n        target, compiler_info, arm_target, rust_supported_targets\n    )/' "$f"
    fi
    grep -q 'MOZ_RUST_TARGET_OVERRIDE' "$f" || {
        echo "error: rust-target override patch no longer applies to $f" >&2
        exit 1
    }
    export MOZ_RUST_TARGET_OVERRIDE=wasm32-wasip1-threads
    rustup target add wasm32-wasip1-threads >/dev/null 2>&1 || true

    f=$SRC/mozglue/misc/moz.build
    perl -0pi -e 's{# WASI hasn.t supported cond vars and mutexes yet so noop implementation is used\.\nelif CONFIG\["OS_ARCH"\] == "WASI":\n    SOURCES \+= \[\n        "ConditionVariable_noop\.cpp",\n        "Mutex_noop\.cpp",\n    \]\n}{# wasi-threads: real mutexes and condvars, on wasi-libc pthreads.\nelif CONFIG["OS_ARCH"] == "WASI":\n    SOURCES += [\n        "ConditionVariable_posix.cpp",\n        "Mutex_posix.cpp",\n        "RWLock_posix.cpp",\n    ]\n}' "$f"
    grep -q 'Mutex_posix.cpp' "$f" || {
        echo "error: WASI mutex patch no longer applies to $f" >&2
        exit 1
    }

    # Atomics.waitAsync timeouts with the INTERNAL job queue: the timeout task
    # goes into internalDelayedDispatchPriorityQueue, but internalDrain's
    # condvar wait has no deadline — nothing wakes it when the delay expires
    # (expired tasks are only flushed when some OTHER dispatch arrives), so a
    # timeout-only waitAsync blocks js::RunJobs forever. Bound the wait by the
    # earliest delayed task's endTime and flush on wake: the drain then wakes
    # exactly when the timeout is due, with no polling. (Upstreamable: any
    # embedding on UseInternalJobQueues hits this; the shell's own event loop
    # masks it.)
    f=$SRC/js/src/vm/OffThreadPromiseRuntimeState.cpp
    if ! grep -q 'wait_until' "$f"; then
        perl -0pi -e 's{      while \(internalDispatchQueue\(\)\.empty\(\)\) \{\n        internalDispatchQueueAppended\(\)\.wait\(lock\);\n      \}}{      while (internalDispatchQueue().empty()) \{\n        auto& delayed = internalDelayedDispatchPriorityQueue();\n        if (!delayed.empty()) \{\n          internalDispatchQueueAppended().wait_until(lock,\n                                                     delayed.highest().endTime());\n          dispatchDelayedTasks();\n        \} else \{\n          internalDispatchQueueAppended().wait(lock);\n        \}\n      \}}' "$f"
    fi
    grep -q 'wait_until' "$f" || {
        echo "error: waitAsync delayed-dispatch patch no longer applies to $f" >&2
        exit 1
    }

    # Companion query: an embedding running its own event loop (each agent's
    # pump here) parks between drains and must know WHEN the next delayed
    # dispatchable (a waitAsync timeout) is due, or it would park forever and
    # the timeout would never be dispatched. Expose the earliest deadline;
    # js.cc bounds each agent's futex wait by it.
    h=$SRC/js/src/vm/OffThreadPromiseRuntimeState.h
    if ! grep -q 'earliestDelayedDispatchMs' "$h"; then
        perl -0pi -e 's/(  bool internalHasPending\(AutoLockHelperThreadState& lock\);\n)/$1\n  \/\/ Milliseconds until the earliest delayed dispatchable (an\n  \/\/ Atomics.waitAsync timeout) is due: 0 if one is already due, -1 if none\n  \/\/ is pending. For embeddings that park between drains.\n  int64_t earliestDelayedDispatchMs();\n/' "$h"
        perl -0pi -e 's/\}  \/\/ namespace js\n\n#endif  \/\/ vm_OffThreadPromiseRuntimeState_h/\/\* wasm2go embedding hooks: pumps parked on their own primitive (a futex)\n \* cannot hear internalDispatchQueueAppended_, so every internal dispatch\n \* also fires this hook. The callback runs under gHelperThreadLock and must\n \* not take locks; spurious wakes are benign. \*\/\nvoid SetWasm2GoDispatchWakeup(void (*fn)());\nvoid Wasm2GoNotifyDispatchWakeup();\n\n\}  \/\/ namespace js\n\n#endif  \/\/ vm_OffThreadPromiseRuntimeState_h/' "$h"
    fi
    grep -q 'earliestDelayedDispatchMs' "$h" || {
        echo "error: delayed-deadline header patch no longer applies to $h" >&2
        exit 1
    }
    grep -q 'SetWasm2GoDispatchWakeup' "$h" || {
        echo "error: dispatch-wakeup header patch no longer applies to $h" >&2
        exit 1
    }
    if ! grep -q 'Wasm2GoEarliestDelayedDispatchMs' "$f"; then
        cat >> "$f" <<'CPPEOF'

int64_t js::OffThreadPromiseRuntimeState::earliestDelayedDispatchMs() {
  AutoLockHelperThreadState lock;
  auto& queue = internalDelayedDispatchPriorityQueue();
  if (queue.empty()) {
    return -1;
  }
  mozilla::TimeStamp now = mozilla::TimeStamp::Now();
  mozilla::TimeStamp end = queue.highest().endTime();
  if (end <= now) {
    return 0;
  }
  return int64_t((end - now).ToMilliseconds()) + 1;
}

namespace js {
int64_t Wasm2GoEarliestDelayedDispatchMs(JSContext* cx) {
  return cx->runtime()->offThreadPromiseState.ref().earliestDelayedDispatchMs();
}

/* An embedding pump parked on its own primitive (each wasm2go agent parks on
 * a futex) cannot hear internalDispatchQueueAppended_. This hook fires after
 * every internal dispatch-queue append, so the embedding can wake its pumps;
 * spurious wakes are benign (a pump re-checks and re-parks). Called under
 * gHelperThreadLock: the callback must not take locks. */
static void (*sWasm2GoDispatchWakeup)() = nullptr;
void SetWasm2GoDispatchWakeup(void (*fn)()) { sWasm2GoDispatchWakeup = fn; }
void Wasm2GoNotifyDispatchWakeup() {
  if (sWasm2GoDispatchWakeup) {
    sWasm2GoDispatchWakeup();
  }
}
}  // namespace js
CPPEOF
    fi
    # Fire the wakeup hook at both append sites (direct dispatch and
    # delayed-task flush share internalDispatchQueueAppended().notify_one()).
    if ! grep -q 'Wasm2GoNotifyDispatchWakeup();' "$f"; then
        perl -0pi -e 's/(  \/\/ Wake up internalDrain\(\) if it is waiting for a job to finish\.\n  state\.internalDispatchQueueAppended\(\)\.notify_one\(\);\n  return true;)/  \/\/ Wake up internalDrain() if it is waiting for a job to finish.\n  state.internalDispatchQueueAppended().notify_one();\n  js::Wasm2GoNotifyDispatchWakeup();\n  return true;/' "$f"
        perl -0pi -e 's/(    internalDispatchQueueAppended\(\)\.notify_one\(\);\n  \}\n\})/    internalDispatchQueueAppended().notify_one();\n    js::Wasm2GoNotifyDispatchWakeup();\n  \}\n\}/' "$f"
    fi
    grep -q 'Wasm2GoEarliestDelayedDispatchMs' "$f" || {
        echo "error: delayed-deadline impl patch no longer applies to $f" >&2
        exit 1
    }
    [[ $(grep -c 'js::Wasm2GoNotifyDispatchWakeup();' "$f") -eq 2 ]] || {
        echo "error: dispatch-wakeup call-site patch no longer applies to $f (want both append sites)" >&2
        exit 1
    }
fi


# gecko's mozinfo assumes a macOS version like "15.3" and indexes the minor
# component; macOS 26 reports a bare "26" and mach dies with IndexError before
# configure even starts. Only bites local (Darwin) builds.
if [[ $(uname -s) == Darwin ]]; then
    f=$SRC/testing/mozbase/mozinfo/mozinfo/mozinfo.py
    perl -0pi -e 's/os_version = f"\{versionNums\[0\]\}\.\{versionNums\[1\]\.ljust\(2, .0.\)\}"/os_version = f"{versionNums[0]}.{(versionNums[1] if len(versionNums) > 1 else \x270\x27).ljust(2, \x270\x27)}"/' "$f"
    grep -q 'if len(versionNums) > 1' "$f" || {
        echo "error: macOS version patch no longer applies to $f" >&2
        exit 1
    }
fi

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
# The threads build must compile EVERY object against wasi-libc's threads
# flavor with the atomics/bulk-memory features on, or wasm-ld later refuses
# --shared-memory. gecko's configure cannot be told --target=wasm32-wasi-threads
# directly (config.sub parses the triple as OS "threads" and rejects it), so
# the mozconfig target stays wasm32-unknown-wasi and the REAL target rides in
# CFLAGS/CXXFLAGS: clang honours the LAST --target on the command line, and
# user flags come after configure's. Verified: a probe object built this way
# carries "+atomics +bulk-memory" in target_features; without, it doesn't.
THREADS_MOZOPTS=""
if [[ -n ${SPIDERMONKEY_THREADS:-} ]]; then
    echo "[engine] threads build (wasm32-wasi-threads sysroot, -pthread)"
    THREADS_MOZOPTS='export CFLAGS="--target=wasm32-wasi-threads -pthread"
export CXXFLAGS="--target=wasm32-wasi-threads -pthread"'
fi

# SPIDERMONKEY_DEBUG=1 builds with assertions (MOZ_ASSERT and friends) for
# hunting memory corruption / race bugs: a debug engine names the broken
# invariant at the point of corruption instead of crashing later. -O2 keeps
# it fast enough to run real workloads; LTO is dropped (slow to link, no
# diagnostic value).
DEBUG_MOZOPTS='ac_add_options --disable-debug
ac_add_options --enable-optimize=-O3
ac_add_options --enable-lto=thin'
ARCHIVE_FLAVOR=release
if [[ -n ${SPIDERMONKEY_DEBUG:-} ]]; then
    echo "[engine] DEBUG build (--enable-debug, -O2, no LTO)"
    DEBUG_MOZOPTS='ac_add_options --enable-debug
ac_add_options --enable-optimize=-O2'
    ARCHIVE_FLAVOR=debug
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
ac_add_options --enable-js-streams
ac_add_options --enable-portable-baseline-interp
ac_add_options --prefix=$OBJ/dist
ac_add_options --with-sysroot=$WASI_SDK_PATH/share/wasi-sysroot
$DEBUG_MOZOPTS
mk_add_options MOZ_OBJDIR=$OBJ
# No AUTOCLOBBER: mach rebuilds incrementally, which is what makes a cached
# objdir worth keeping. A configure-affecting change still triggers its own
# reconfigure.
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
    mozglue/misc/Debug.o
    mozglue/misc/Decimal.o
    mozglue/misc/MmapFaultHandler.o
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

# The mutex/condvar objects depend on the flavor: the single-agent build gets
# mozglue's noop implementations, a threads build the real posix ones (which is
# what the moz.build patch above selects).
if [[ -n ${SPIDERMONKEY_THREADS:-} ]]; then
    SM_OBJ_FILES+=(
        mozglue/misc/ConditionVariable_posix.o
        mozglue/misc/Mutex_posix.o
        mozglue/misc/RWLock_posix.o
    )
else
    SM_OBJ_FILES+=(
        mozglue/misc/ConditionVariable_noop.o
        mozglue/misc/Mutex_noop.o
    )
fi

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
# Pick the staticlib for THIS build's rust target, not `find | head -1`: a
# cached objdir can hold both wasm32-wasip1/ and wasm32-wasip1-threads/, and
# shipping the non-threads jsrust in a threads engine reintroduces the exact
# wasm-ld --shared-memory rejection the threads rust target exists to fix.
if [[ -n ${SPIDERMONKEY_THREADS:-} ]]; then
    JSRUST=$OBJ/wasm32-wasip1-threads/release/libjsrust.a
else
    JSRUST=$OBJ/wasm32-wasip1/release/libjsrust.a
fi
[[ -f $JSRUST ]] || JSRUST=$(find "$OBJ" -name 'libjsrust.a' | head -1)
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
mkdir -p "$repo_root/build"
tar -czf "$repo_root/build/spidermonkey-static-intl-$ARCHIVE_FLAVOR.tar.gz" \
    -C "$PKG/.." "$(basename "$PKG")" \
    --transform "s|^$(basename "$PKG")|spidermonkey-dist-intl-release|"
echo "[engine] wrote build/spidermonkey-static-intl-$ARCHIVE_FLAVOR.tar.gz"
ls -lh "$repo_root/build/spidermonkey-static-intl-$ARCHIVE_FLAVOR.tar.gz" "$PKG/libspidermonkey.a"
