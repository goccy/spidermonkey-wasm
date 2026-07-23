/* js.cc — SpiderMonkey embedding bridge (implements js.h) for wasm32-wasi + Go.
 *
 * This is the wasmify CustomBridgeSource: it wraps libspidermonkey.a behind the
 * tiny js.h surface (js_new / js_eval / js_close / js_interrupt_*) that the
 * generator exports to Go.
 *
 * It depends on SpiderMonkey's PUBLIC JSAPI only — the prebuilt distribution
 * ships no internal headers. The one place that would want an internal header
 * is locating JSContext::interruptBits_; discover_interrupt_bits() finds it at
 * runtime instead, and the runtime degrades gracefully when it cannot.
 *
 * Threading: one wasm instance == one JSContext. A second runtime means a
 * second wasm2go module instance with its own linear memory and its own copy of
 * these globals.
 */

/* js-confdefs.h carries the configuration the library was compiled with (value
 * representation, GC constants, ...). SpiderMonkey requires it before any of
 * its other headers: get the object layout wrong and every call across the
 * boundary silently corrupts. Including it here rather than passing
 * `-include js-confdefs.h` keeps the requirement next to the code it protects. */
#include "js-confdefs.h"

#include "js.h"

#include <jsapi.h>
/* js::UseInternalJobQueues / js::RunJobs — the internal microtask queue an
 * embedding without its own event loop uses. */
#include <jsfriendapi.h>

#include <js/Array.h>
#include <js/ArrayBuffer.h>
#include <js/experimental/TypedData.h>
#include <js/HelperThreadAPI.h>
#include <js/SharedArrayBuffer.h>
#include <js/StructuredClone.h>
#include <js/CompilationAndEvaluation.h>
#include <js/Context.h>
#include <js/Conversions.h>
#include <js/ErrorReport.h>
#include <js/Exception.h>
#include <js/GCAPI.h>
#include <js/GlobalObject.h>
#include <js/CharacterEncoding.h>
#include <js/Initialization.h>
#include <js/Interrupt.h>
#include <js/JSON.h>
#include <js/Modules.h>
#include <js/Object.h>
#include <js/Promise.h>
#include <js/PropertyAndElement.h>
#include <js/Realm.h>
#include <js/RootingAPI.h>
#include <js/ScriptPrivate.h>
#include <js/SourceText.h>
#include <js/Stack.h>
#include <js/Warnings.h>
#include <js/CallAndConstruct.h>
#include <js/ValueArray.h>
#include <js/GCVector.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#ifdef SPIDERMONKEY_WASM_THREADS
#include <pthread.h>
#endif

/* ---- runtime state ----------------------------------------------------------
 *
 * NO process globals for per-runtime state. js_new heap-allocates a Runtime and
 * returns its ADDRESS as the opaque handle every js_* export takes; JSNatives
 * (which receive only a cx) reach it through JS_GetContextPrivate. The only
 * process-level statics left are genuinely process-scoped: the JS_Init once
 * flag, the helper-thread/dispatch callback registrations, the agent wake
 * futex, and the clone-buffer mutex. */

/* A registered ES module: source, plus the lazily compiled records (see the
 * ES-modules section). */
struct ModuleEntry {
    std::string source;
    JS::PersistentRootedObject *js = nullptr;   /* ModuleType::JavaScript */
    JS::PersistentRootedObject *json = nullptr; /* ModuleType::JSON */
};

#ifdef SPIDERMONKEY_WASM_THREADS
/* Agent lifecycle state (see the agents section). Communication policy lives
 * host-side; this is exactly what js_close's deterministic shutdown needs. */
struct AgentState {
    std::mutex mu;
    std::atomic<bool> shutdown{false};
    std::atomic<uint32_t> alive{0}; /* running agent threads; futex-signaled on exit */
    std::atomic<uint64_t> next_id{1};
    std::vector<pthread_t> threads;
    std::vector<JSContext *> contexts;
};
#endif

struct Runtime;

/* Per-CONTEXT data, reachable via JS_GetContextPrivate: the main context and
 * every agent context carry one. It holds what is genuinely context-scoped —
 * the print/console capture (each context is single-threaded, so no locking),
 * the context's setTimeout queue, and the agent-only bits. */
struct CtxData {
    Runtime *rt = nullptr;
#ifdef SPIDERMONKEY_WASM_THREADS
    bool agent_left = false; /* __agent_leaving__() was called */
    uint64_t agent_id = 0;   /* 0 on the main context */
#endif
};

/* One runtime — one JSContext, one global, one handle. */
struct Runtime {
    JSContext *cx = nullptr;
    /* Heap-allocated so its destructor never runs after the context is gone (a
     * PersistentRooted must not outlive its runtime). */
    JS::PersistentRootedObject *global = nullptr;

    /* The main context's private data (capture buffers). */
    CtxData main_ctx;

    /* Set by the host (a plain 32-bit store into linear memory at
     * js_interrupt_addr) to mean "the host asked for this interrupt".
     * `volatile` so the interrupt callback always reloads it. */
    volatile uint32_t interrupt = 0;
    /* &JSContext::interruptBits_ and the bit JS_RequestInterruptCallback sets
     * in it, as established by discover_interrupt_bits(). Zero means discovery
     * failed and the fallback (keep the interrupt permanently armed) is in
     * force. */
    uint32_t *bits_addr = nullptr;
    uint32_t bits_value = 0;
    /* True only while discover_interrupt_bits() is probing, so the callback
     * neither terminates the (nonexistent) script nor re-arms and perturbs the
     * very word being measured. */
    bool discovering = false;

    /* Registered ES modules, keyed by specifier (see the ES-modules section).
     * The registry is the loader's per-runtime CACHE: sources arrive through
     * the reserved module-load host call and are compiled lazily per import. */
    std::map<std::string, ModuleEntry> *modules = nullptr;

#ifdef SPIDERMONKEY_WASM_THREADS
    /* The runtime agents parent themselves to — same agent cluster, which is
     * what makes the SharedArrayBuffer waiter list and off-thread promise
     * bookkeeping (both cluster-scoped) reach them. */
    JSRuntime *jsrt = nullptr;
    AgentState agents;
#endif
};

static Runtime *rt_from(uint64_t h) { return reinterpret_cast<Runtime *>(h); }

static CtxData *ctx_data(JSContext *cx) {
    return static_cast<CtxData *>(JS_GetContextPrivate(cx));
}

static Runtime *rt_of(JSContext *cx) {
    CtxData *d = ctx_data(cx);
    return d ? d->rt : nullptr;
}

/* ---- interrupt ----------------------------------------------------------- */

/* Invoked by the engine at a bytecode loop head once interruptBits_ is nonzero.
 *
 * Returning false makes HandleInterrupt (js/src/vm/Runtime.cpp) report an
 * UNCATCHABLE exception and unwind: JS::Evaluate then returns false with NO
 * pending exception, and no `catch` or `finally` in the guest script can
 * intercept it.
 *
 * Returning true resumes the script. The engine clears interruptBits_ BEFORE
 * calling us, so a resume must re-arm or we will never be polled again.
 *
 * rt->interrupt, not the mere fact of being called, is what authorises
 * termination, and re-arming on every non-authorised call is what makes that
 * safe. Two things could otherwise call us without the host having asked:
 *
 *   - The engine itself. In a release build nothing does: the only
 *     `requestInterrupt(CallbackUrgent/CallbackCanWait)` calls outside
 *     JS_RequestInterruptCallback are under `#ifdef DEBUG` (RegExpObject.cpp's
 *     simulated interrupt) or the fuzzing-only JS_INTERRUPT_POSSIBLY_FAIL
 *     (js/public/Utility.h). We link the release archive. But a future engine
 *     could add one, and terminating a script on the engine's own GC-adjacent
 *     interrupt would be a silent, unreproducible abort.
 *
 *   - The host, momentarily. Fire() stores rt->interrupt before the interruptBits_
 *     bit, but the guest runs on another thread and reads the two words with
 *     independent, unordered loads; it can observe the bit while rt->interrupt is
 *     still stale. Re-arming and resuming turns that into a one-loop-head delay
 *     rather than a lost interrupt — and a lost interrupt means Eval never
 *     returns while the host believes it cancelled the script.
 *
 * Re-arming can therefore only spin for as long as the flag takes to become
 * visible. It cannot spin forever unless something trips interruptBits_ and
 * never sets rt->interrupt, which nothing in this design does. */
static bool interrupt_cb(JSContext *cx) {
    Runtime *rt = rt_of(cx);
    if (!rt || rt->discovering) {
        return true;
    }
#ifdef SPIDERMONKEY_WASM_THREADS
    /* js_close interrupts every agent context (CanWait) so an agent parked
     * inside the engine — an Atomics.wait with time left — unblocks NOW.
     * Without this arm the callback would judge the interrupt "not ours",
     * re-arm, and RESUME the wait: close would stall for the remaining
     * timeout. Terminate the agent script instead (uncatchable, like the
     * host interrupt). */
    if (rt->agents.shutdown.load(std::memory_order_seq_cst)) {
        return false;
    }
#endif
    if (rt->interrupt) {
        rt->interrupt = 0;
        return false;
    }
    JS_RequestInterruptCallback(cx);
    return true;
}

/* Locate JSContext::interruptBits_ using the public API only.
 *
 * JS_RequestInterruptCallback's entire effect on JSContext is
 * `interruptBits_ |= uint32_t(reason)` plus `jitStackLimit = NativeStackLimitMin`
 * (js/src/vm/Runtime.cpp, JSContext::requestInterrupt). So: snapshot the object,
 * request an interrupt, and see which word changed. Two probes with two
 * different reasons pin it down:
 *
 *   - the word must go 0 -> v, with v a power of two (one InterruptReason bit),
 *   - handling the interrupt must return it to 0 (handleInterrupt clears it),
 *   - and the two probes must yield DIFFERENT values, because CallbackUrgent and
 *     CallbackCanWait are distinct bits. jitStackLimit fails the first test (its
 *     prior value is the real stack limit, not 0) and any word that merely
 *     toggles fails the third.
 *
 * The bit value is read back from what SpiderMonkey wrote rather than hardcoded,
 * so a reordering of the InterruptReason enum cannot silently break us.
 *
 * Must run inside a realm: handling an interrupt touches cx->realm().
 *
 * On success rt->bits_addr/rt->bits_value are set and the host can trip the poll
 * with a single store, leaving the steady-state cost at the one relaxed load the
 * interpreter already does per loop head. On failure they stay 0 and js_new arms
 * the interrupt permanently instead — correct, just slower. */
static void discover_interrupt_bits(JSContext *cx) {
    Runtime *rt = rt_of(cx);
    /* Test hook: forcing discovery to fail is the only way to exercise the
     * always-armed fallback, which would otherwise be code that never runs
     * until the day a SpiderMonkey upgrade moves interruptBits_. */
    if (!rt || std::getenv("SPIDERMONKEY_WASM_NO_INTERRUPT_DISCOVERY")) {
        return;
    }

    /* JSContext is a few KB; interruptBits_ sits well inside it. Reading past
     * the object would still be safe (it is heap memory in a wasm linear
     * address space with no guard pages) but there is no reason to. */
    constexpr size_t kScanWords = 1024; /* 4 KiB */
    auto *words = reinterpret_cast<volatile uint32_t *>(cx);

    static uint32_t before[kScanWords];
    static uint32_t probe1[kScanWords];

    auto is_pow2 = [](uint32_t v) { return v != 0 && (v & (v - 1)) == 0; };

    rt->discovering = true;

    for (size_t i = 0; i < kScanWords; i++) {
        before[i] = words[i];
    }

    JS_RequestInterruptCallback(cx); /* sets InterruptReason::CallbackUrgent */
    for (size_t i = 0; i < kScanWords; i++) {
        probe1[i] = words[i];
    }
    /* Clears interruptBits_ and runs our callback, which is a no-op right now. */
    JS_CheckForInterrupt(cx);

    JS_RequestInterruptCallbackCanWait(cx); /* sets InterruptReason::CallbackCanWait */
    uint32_t *found = nullptr;
    uint32_t found_value = 0;
    size_t matches = 0;
    for (size_t i = 0; i < kScanWords; i++) {
        const uint32_t v2 = words[i];
        if (before[i] != 0 || !is_pow2(probe1[i]) || !is_pow2(v2) || probe1[i] == v2) {
            continue;
        }
        matches++;
        found = const_cast<uint32_t *>(&words[i]);
        found_value = probe1[i];
    }
    JS_CheckForInterrupt(cx);

    rt->discovering = false;

    /* Ambiguity is indistinguishable from a wrong guess, so refuse both: a
     * mislocated word would make the host scribble into unrelated engine state.
     * Exactly one survivor, and it must be back at rest. */
    if (matches != 1 || *found != 0) {
        return;
    }
    rt->bits_addr = found;
    rt->bits_value = found_value;
}

#ifdef SPIDERMONKEY_WASM_THREADS
/* ---- helper-thread pool ----------------------------------------------------
 * A threads build hands internal tasks to an EXTERNAL pool and waits on them.
 * Each task runs on its own pthread — a goroutine under wasm2go. Registered
 * BEFORE JS_Init: the engine decides useInternalThreadPool_ from whether a
 * callback is already set. */
static void *helper_thread_trampoline(void *arg) {
    JS::RunHelperThreadTask(static_cast<JS::HelperThreadTask *>(arg));
    return nullptr;
}

static void helper_thread_dispatch(JS::HelperThreadTask *task) {
    pthread_t tid;
    if (pthread_create(&tid, nullptr, helper_thread_trampoline, task) != 0) {
        JS::RunHelperThreadTask(task); /* inline: a dropped task deadlocks */
        return;
    }
    pthread_detach(tid);
}
#endif /* SPIDERMONKEY_WASM_THREADS */

static bool host_func_call(JSContext *cx, unsigned argc, JS::Value *vp);

/* JS value <-> Go value codec (defined with the object-handle bindings below);
 * host_func_call marshals through it so argument/return identity survives. */
static void encode_js_value(JSContext *cx, JS::HandleValue val, std::string &out);
static bool parse_json_utf8(JSContext *cx, const char *data, size_t len,
                            JS::MutableHandleValue out);
static void decode_js_value(JSContext *cx, JS::HandleValue encoded, JS::MutableHandleValue out);

/* The guest->host transport a host function's calls travel over: a pair of env
 * imports the embedding provides.
 *
 *   go_host_call(key, key_len, args, args_len, this_id, out, out_cap) -> total
 *     Invokes the host handler ONCE. If the reply fits out_cap it is already
 *     in `out`; otherwise the host stages it (per instance) and the guest
 *     re-fetches after growing — the handler is never invoked twice, and
 *     nothing here re-enters the wasm instance from host code.
 *   go_host_result(out)
 *     Copies the staged reply and clears it. */
extern "C" {
__attribute__((import_module("env"), import_name("go_host_call")))
uint32_t go_host_call(const char *key, uint32_t key_len, const char *args,
                      uint32_t args_len, uint64_t this_id, char *out,
                      uint32_t out_cap);
__attribute__((import_module("env"), import_name("go_host_result")))
void go_host_result(char *out);
}

static bool host_func_call(JSContext *cx, unsigned argc, JS::Value *vp) {
    JS::CallArgs args = JS::CallArgsFromVp(argc, vp);

    /* The dispatch key lives in the function's native reserved slot —
     * guest code cannot rename it out from under the host. */
    JS::RootedObject calleeObj(cx, &args.callee());
    JS::RootedValue keyVal(cx, js::GetFunctionNativeReserved(calleeObj, 0));
    if (!keyVal.isString()) {
        JS_ReportErrorASCII(cx, "host function has no dispatch key");
        return false;
    }
    JS::RootedString nameStr(cx, keyVal.toString());
    JS::UniqueChars nameUtf8 = JS_EncodeStringToUTF8(cx, nameStr);
    if (!nameUtf8) {
        return false;
    }
    /* Take the ENCODED length, not strlen: a dispatch key may legally contain
     * NUL bytes (the host's reserved-key namespace is NUL-prefixed precisely
     * so guest-visible names can never collide with it), and the C-string
     * constructor would silently truncate the key to "" at the first one. */
    JSLinearString *nameLin = JS_EnsureLinearString(cx, nameStr);
    if (!nameLin) {
        return false;
    }
    std::string name(nameUtf8.get(), JS::GetDeflatedUTF8StringLength(nameLin));

    /* Arguments as a JSON array of value ENCODINGS — primitives carry their
     * data, objects and functions carry a persistent handle. JSON.stringify
     * here would destroy identity (an object argument would arrive as a
     * detached copy, a function argument as null); the encoding hands Go the
     * same object, navigable and callable. */
    std::string argsJson = "[";
    for (unsigned i = 0; i < args.length(); i++) {
        if (i) {
            argsJson += ",";
        }
        encode_js_value(cx, args[i], argsJson);
    }
    argsJson += "]";

    std::vector<char> out(4096);
    uint32_t total = go_host_call(name.data(), (uint32_t)name.size(), argsJson.data(),
                                  (uint32_t)argsJson.size(), 0, out.data(),
                                  (uint32_t)out.size());
    if (total > out.size()) {
        out.resize(total);
        go_host_result(out.data());
    }
    if (total == 0) {
        JS_ReportErrorUTF8(cx, "host function not registered: %s", name.c_str());
        return false;
    }
    char tag = out[0];
    std::string payload(out.data() + 1, total - 1);
    if (tag == 'E') {
        JS_ReportErrorUTF8(cx, "%s", payload.c_str());
        return false;
    }
    /* 'R' (return) and 'T' (throw) both carry one value encoding; decode it
     * back into a live value — a primitive from its data, an object/function
     * from its handle. 'T' lets a host function throw an arbitrary JS value
     * (e.g. a SyntaxError instance) with its type intact, not just a message. */
    JS::RootedValue encoded(cx);
    if (!parse_json_utf8(cx, payload.data(), payload.size(), &encoded)) {
        JS_ReportErrorASCII(cx, "undecodable host function result");
        return false;
    }
    JS::RootedValue rv(cx);
    decode_js_value(cx, encoded, &rv);
    if (tag == 'T') {
        JS_SetPendingException(cx, rv);
        return false;
    }
    args.rval().set(rv);
    return true;
}

/* Warnings (including the "terminated" warning HandleInterrupt emits when our
 * callback returns false) are swallowed: the engine ships no I/O, and console
 * is a host opt-in, so there is nowhere for an engine warning to go. */
static void warning_reporter(JSContext *cx, JSErrorReport *report) {
    (void)cx;
    (void)report;
}

/* ---- JSON helpers (same shape as perl-wasm's perl.cc) -------------------- */

static void json_escape(const std::string &in, std::string &out) {
    for (unsigned char c : in) {
        switch (c) {
        case '"':  out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n";  break;
        case '\r': out += "\\r";  break;
        case '\t': out += "\\t";  break;
        case '\b': out += "\\b";  break;
        case '\f': out += "\\f";  break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            } else {
                out += static_cast<char>(c);
            }
        }
    }
}

static std::string json_field(const char *key, const std::string &val, bool comma) {
    std::string s = "\"";
    s += key;
    s += "\":\"";
    json_escape(val, s);
    s += "\"";
    if (comma) s += ",";
    return s;
}


static std::string make_result(bool ok, const std::string &result,
                               const std::string &error) {
    std::string j = "{\"ok\":";
    j += ok ? "true" : "false";
    j += ",";
    j += json_field("result", result, true);
    j += json_field("error", error, false);
    j += "}";
    return j;
}

/* ---- error formatting ---------------------------------------------------- */

/* Best-effort UTF-8 of a value; empty string when the value itself throws while
 * stringifying (we are already on an error path, so we swallow that). */
static std::string to_utf8(JSContext *cx, JS::HandleValue v) {
    JS::RootedString str(cx, JS::ToString(cx, v));
    if (!str) {
        JS_ClearPendingException(cx);
        return std::string();
    }
    JS::UniqueChars utf8 = JS_EncodeStringToUTF8(cx, str);
    if (!utf8) {
        JS_ClearPendingException(cx);
        return std::string();
    }
    return std::string(utf8.get());
}

/* Drain the pending exception into a message (+ stack when the engine captured
 * one). A false return from JS::Evaluate with NOTHING pending is the uncatchable
 * termination our interrupt callback triggers. */
static std::string take_error(JSContext *cx) {
    if (!JS_IsExceptionPending(cx)) {
        return std::string("JS execution interrupted");
    }
    JS::ExceptionStack es(cx);
    if (!JS::StealPendingExceptionStack(cx, &es)) {
        JS_ClearPendingException(cx);
        return std::string("uncaught exception (could not be retrieved)");
    }
    JS::RootedValue exc(cx, es.exception());
    std::string msg = to_utf8(cx, exc);

    JS::RootedObject stack(cx, es.stack());
    if (stack) {
        JS::RootedString stackStr(cx);
        if (JS::BuildStackString(cx, nullptr, stack, &stackStr, 0)) {
            JS::UniqueChars utf8 = JS_EncodeStringToUTF8(cx, stackStr);
            if (utf8 && *utf8.get()) {
                msg += "\n";
                msg += utf8.get();
            }
        }
        JS_ClearPendingException(cx);
    }
    return msg;
}

/* ---- ES modules ----------------------------------------------------------- */

/* Registered modules, keyed by specifier. The registry stores SOURCE and
 * compiles lazily, per requested module type, on first import:
 *  - a compile error then surfaces AT IMPORT TIME with its real type (a
 *    dynamic import of script-only code must reject with SyntaxError, not
 *    with a host "could not register" error), and
 *  - one registered source can serve both as a JS module and as a JSON
 *    module (import attributes decide, per request).
 * Compiled records are heap-allocated PersistentRooteds released in js_close
 * (they must not outlive the runtime). The registry IS the loader: the host
 * resolves specifiers to sources and registers them; the guest never does
 * I/O (see js.h). */

/* Resolve ./ and ../ in `spec` against the registry key of the importing
 * module. Bare and absolute-looking specifiers are exact registry keys. */
static std::string resolve_specifier(const std::string &spec, const std::string &referrer) {
    if (spec.rfind("./", 0) != 0 && spec.rfind("../", 0) != 0) {
        return spec;
    }
    std::string base;
    size_t slash = referrer.rfind('/');
    if (slash != std::string::npos) {
        base = referrer.substr(0, slash);
    }
    /* Split base + spec into segments, dropping "." and folding "..". */
    std::vector<std::string> segs;
    auto push = [&segs](const std::string &s) {
        size_t start = 0;
        while (start <= s.size()) {
            size_t end = s.find('/', start);
            if (end == std::string::npos) {
                end = s.size();
            }
            std::string seg = s.substr(start, end - start);
            if (seg == "..") {
                if (!segs.empty()) {
                    segs.pop_back();
                }
            } else if (!seg.empty() && seg != ".") {
                segs.push_back(seg);
            }
            start = end + 1;
        }
    };
    push(base);
    push(spec);
    std::string out;
    for (size_t i = 0; i < segs.size(); i++) {
        if (i) {
            out += '/';
        }
        out += segs[i];
    }
    return out;
}

static std::string jsstring_to_utf8(JSContext *cx, JS::HandleString str) {
    JS::UniqueChars utf8 = JS_EncodeStringToUTF8(cx, str);
    if (!utf8) {
        JS_ClearPendingException(cx);
        return std::string();
    }
    return std::string(utf8.get());
}

/* Store src under specifier; compilation happens lazily at import. */
static void register_module_source(Runtime *rt, const std::string &specifier,
                                   const std::string &src) {
    if (!rt->modules) {
        rt->modules = new std::map<std::string, ModuleEntry>();
    }
    ModuleEntry &e = (*rt->modules)[specifier];
    delete e.js;
    delete e.json;
    e = ModuleEntry{};
    e.source = src;
}

/* Compile the entry's source for the requested module type (caching the
 * record) and tag it with its specifier — the private value the load hook and
 * the import.meta hook read back. A compile error stays pending so the import
 * fails with its REAL type (e.g. SyntaxError for script-only source). */
static JSObject *compile_module_entry(JSContext *cx, const std::string &specifier,
                                      ModuleEntry &e, JS::ModuleType type) {
    JS::PersistentRootedObject *&slot = (type == JS::ModuleType::JSON) ? e.json : e.js;
    if (slot) {
        return slot->get();
    }
    JS::CompileOptions opts(cx);
    opts.setFileAndLine(specifier.c_str(), 1);
    JS::SourceText<mozilla::Utf8Unit> buf;
    if (!buf.init(cx, e.source.data(), e.source.size(), JS::SourceOwnership::Borrowed)) {
        return nullptr;
    }
    JS::RootedObject module(cx, type == JS::ModuleType::JSON
                                    ? JS::CompileJsonModule(cx, opts, buf)
                                    : JS::CompileModule(cx, opts, buf));
    if (!module) {
        return nullptr;
    }
    JS::RootedString specStr(cx, JS_NewStringCopyN(cx, specifier.data(), specifier.size()));
    if (!specStr) {
        return nullptr;
    }
    JS::SetModulePrivate(module, JS::StringValue(specStr));
    slot = new JS::PersistentRootedObject(cx, module);
    return module;
}

/* Register + compile eagerly as a JS module (js_eval_module's entry path,
 * where an immediate compile error is the caller's answer). */
static JSObject *compile_and_register_module(JSContext *cx, const std::string &specifier,
                                             const std::string &src) {
    Runtime *rt = rt_of(cx);
    register_module_source(rt, specifier, src);
    return compile_module_entry(cx, specifier, (*rt->modules)[specifier],
                                JS::ModuleType::JavaScript);
}

/* Ask the Go host to load a module's source by specifier. Reuses the
 * go_host_call channel under a reserved, NUL-prefixed key so it can never
 * collide with a host function name. Returns true with out_source set when the
 * loader supplied source; false when no loader is registered (total == 0) so the
 * caller falls back to the missing-modules protocol; and false WITH a pending
 * exception when the loader reported an error. Unlike host_func_call the reply
 * carries the raw source, not JSON — the source is bytes, not a value. */
static bool call_go_module_loader(JSContext *cx, const std::string &specifier,
                                  const std::string &referrer, std::string &out_source) {
    static const std::string key("\0module-load", 12);
    std::string args = "[\"";
    json_escape(specifier, args);
    args += "\",\"";
    json_escape(referrer, args);
    args += "\"]";

    std::vector<char> out(4096);
    uint32_t total = go_host_call(key.data(), (uint32_t)key.size(), args.data(),
                                  (uint32_t)args.size(), 0, out.data(),
                                  (uint32_t)out.size());
    if (total > out.size()) {
        out.resize(total);
        go_host_result(out.data());
    }
    if (total == 0) {
        return false; /* no loader registered */
    }
    char tag = out[0];
    if (tag != 'R') {
        /* 'E': the loader failed. Surface its message so the import fails with a
         * real reason; the pending exception tells lookup_module to stop. */
        std::string msg(out.data() + 1, total - 1);
        JS_ReportErrorUTF8(cx, "%s", msg.c_str());
        return false;
    }
    out_source.assign(out.data() + 1, total - 1);
    return true;
}

/* Look up (and lazily compile) the module a request resolves to. On a registry
 * miss, ask the Go loader (if one is registered) to fetch the source; only when
 * there is no loader does it fall back to reporting a "module not registered"
 * error and recording the specifier for the missing-modules retry protocol. */
static JSObject *lookup_module(JSContext *cx, JS::HandleObject moduleRequest,
                               JS::Handle<JSScript *> referrer) {
    JS::RootedString specStr(cx, JS::GetModuleRequestSpecifier(cx, moduleRequest));
    if (!specStr) {
        return nullptr;
    }
    std::string spec = jsstring_to_utf8(cx, specStr);
    std::string ref;
    if (referrer) {
        JS::RootedValue priv(cx, JS::GetScriptPrivate(referrer));
        if (priv.isString()) {
            JS::RootedString refStr(cx, priv.toString());
            ref = jsstring_to_utf8(cx, refStr);
        }
    }
    std::string resolved = resolve_specifier(spec, ref);
    JS::ModuleType type = JS::GetModuleRequestType(cx, moduleRequest);
    Runtime *rt = rt_of(cx);
    if (rt->modules) {
        auto it = rt->modules->find(resolved);
        if (it != rt->modules->end()) {
            return compile_module_entry(cx, resolved, it->second, type);
        }
    }
    std::string source;
    if (call_go_module_loader(cx, resolved, ref, source)) {
        register_module_source(rt, resolved, source);
        return compile_module_entry(cx, resolved, (*rt->modules)[resolved], type);
    }
    if (JS_IsExceptionPending(cx)) {
        return nullptr; /* the loader failed; keep its exception */
    }
    JS_ReportErrorUTF8(cx, "module not registered: %s", resolved.c_str());
    return nullptr;
}

static bool load_module_resolved(JSContext *cx, JS::Handle<JS::Value> hostDefined) {
    /* Mirrors the shell's ModuleLoader::LoadResolved: once a dynamically
     * imported module's dependency graph is loaded, LINK it — but only when
     * it still needs linking. Spec Link (16.2.1.5.1) THROWS for a module in
     * Linking/Evaluating status, and a SELF-importing module is Evaluating at
     * exactly this moment; it is already linked, so skip. */
    JS::RootedObject module(cx, &hostDefined.toObject());
    if (JS::ModuleIsLinked(module)) {
        return true;
    }
    return JS::ModuleLink(cx, module);
}

static bool load_module_rejected(JSContext *cx, JS::Handle<JS::Value> hostDefined,
                                 JS::Handle<JS::Value> error) {
    (void)hostDefined;
    /* Re-raise so the caller's take_error sees the real reason. */
    JS_SetPendingException(cx, error);
    return true;
}

/* HostGetImportMetaProperties: this embedding defines one property, url —
 * the module's registry specifier (its module private). */
static bool module_metadata(JSContext *cx, JS::Handle<JS::Value> privateValue,
                            JS::Handle<JSObject *> metaObject) {
    JS::RootedValue url(cx);
    if (privateValue.isString()) {
        url = privateValue;
    } else {
        url = JS::StringValue(JS_GetEmptyString(cx));
    }
    return JS_DefineProperty(cx, metaObject, "url", url, JSPROP_ENUMERATE);
}

/* HostLoadImportedModule: serves BOTH static imports (during
 * LoadRequestedModules) and dynamic import() (payload is the promise). Mirrors
 * js/src/shell/ModuleLoader.cpp, with the filesystem replaced by the registry. */
static bool load_imported_module(JSContext *cx, JS::Handle<JSScript *> referrer,
                                 JS::HandleObject moduleRequest,
                                 JS::HandleValue hostDefined, JS::HandleValue payload,
                                 uint32_t lineNumber, JS::ColumnNumberOneOrigin columnNumber) {
    (void)hostDefined;
    (void)lineNumber;
    (void)columnNumber;

    JS::RootedObject payloadObj(cx, payload.isObject() ? &payload.toObject() : nullptr);
    const bool dynamic = payloadObj && JS::IsPromiseObject(payloadObj);

    JS::RootedObject module(cx, lookup_module(cx, moduleRequest, referrer));
    if (!module) {
        if (dynamic) {
            return JS::FinishLoadingImportedModuleFailedWithPendingException(cx, payload);
        }
        return false; /* pending exception; the engine runs the failure path */
    }

    if (dynamic) {
        /* A dynamically imported module's own dependency graph has not been
         * loaded yet; kick that off before handing the module back. */
        JS::RootedValue hd(cx, JS::ObjectValue(*module));
        if (!JS::LoadRequestedModules(cx, module, hd, load_module_resolved,
                                      load_module_rejected) ||
            JS_IsExceptionPending(cx)) {
            return JS::FinishLoadingImportedModuleFailedWithPendingException(cx, payload);
        }
        /* usePromise: continue the dynamic import from a promise JOB, not
         * synchronously inside this hook. A module importing ITSELF is
         * Evaluating right now; the spec only reaches ContinueDynamicImport
         * after the current evaluation job, when the status is Evaluated —
         * continuing synchronously evaluates an Evaluating record and dies
         * with "module record has unexpected status". */
        return JS::FinishLoadingImportedModule(cx, nullptr, moduleRequest, payload, module,
                                               /* usePromise = */ true);
    }
    return JS::FinishLoadingImportedModule(cx, referrer, moduleRequest, payload, module,
                                           /* usePromise = */ false);
}

/* Create a forwarding stub whose native reserved slot 0 carries `key`. */
static JSObject *new_host_stub(JSContext *cx, const std::string &key,
                               const std::string &name, unsigned nargs, unsigned flags,
                               JSNative native) {
    // nargs is the function's declared arity (its `length`); it does not limit
    // how many arguments a call may pass — every JS function is variadic and
    // host_func_call reads them all — it only sets the advertised `length`.
    // flags carries JSFUN_* bits (e.g. JSFUN_CONSTRUCTOR for a `new`-able stub).
    JSFunction *fn = js::NewFunctionWithReserved(cx, native, nargs, flags, name.c_str());
    if (!fn) {
        return nullptr;
    }
    JS::RootedObject fnObj(cx, JS_GetFunctionObject(fn));
    JS::RootedString keyStr(cx, JS_NewStringCopyN(cx, key.data(), key.size()));
    if (!keyStr) {
        return nullptr;
    }
    js::SetFunctionNativeReserved(fnObj, 0, JS::StringValue(keyStr));
    return fnObj;
}

/* ---- raw handles -----------------------------------------------------------
 * A handle is a uint64 that is really a JS::PersistentRooted<JS::Value>* on the
 * C++ heap. SpiderMonkey's GC moves and collects its things, so a raw pointer
 * cannot be handed across the bridge and held: the persistent root is a stable
 * cell the GC keeps valid (updated on move) and alive. One handle type serves
 * every GC-managed value — objects and functions, but also symbols and bigints,
 * which have no host data representation and so must cross by handle too.
 * js_free_object deletes the cell; the Go side drives that from a finalizer. */
static uint64_t new_val_handle(JSContext *cx, JS::HandleValue v) {
    return reinterpret_cast<uint64_t>(new JS::PersistentRooted<JS::Value>(cx, v));
}
static JS::Value val_from_handle(uint64_t handle) {
    return handle ? reinterpret_cast<JS::PersistentRooted<JS::Value> *>(handle)->get()
                  : JS::UndefinedValue();
}
static uint64_t new_obj_handle(JSContext *cx, JSObject *obj) {
    if (!obj) {
        return 0;
    }
    JS::RootedValue v(cx, JS::ObjectValue(*obj));
    return new_val_handle(cx, v);
}
static JSObject *obj_from_handle(uint64_t handle) {
    JS::Value v = val_from_handle(handle);
    return v.isObject() ? &v.toObject() : nullptr;
}

uint64_t js_global(uint64_t h) {
    Runtime *rt = rt_from(h);
    if (!rt || !rt->cx || !rt->global) {
        return 0;
    }
    return new_obj_handle(rt->cx, rt->global->get());
}

uint64_t js_new_plain_object(uint64_t h) {
    Runtime *rt = rt_from(h);
    if (!rt || !rt->cx) {
        return 0;
    }
    JS::RootedObject global(rt->cx, rt->global->get());
    JSAutoRealm ar(rt->cx, global);
    return new_obj_handle(rt->cx, JS_NewPlainObject(rt->cx));
}

/* Define a host function (dispatched to Go by `key`) on the object `obj_handle`
 * names. Calls dispatch to the Go function the embedder registered under
 * `key`. */
std::string js_define_function(uint64_t h, uint64_t obj_handle, const char *name_p,
                               uint32_t name_len, const char *key_p, uint32_t key_len,
                               uint32_t nargs) {
    Runtime *rt = rt_from(h);
    if (!rt || !rt->cx) {
        return "{\"ok\":false,\"result\":\"\",\"error\":\"no runtime\"}";
    }
    JS::RootedObject obj(rt->cx, obj_from_handle(obj_handle));
    if (!obj) {
        return make_result(false, "", "invalid object handle");
    }
    const std::string name(name_p ? name_p : "", name_p ? name_len : 0);
    const std::string key(key_p ? key_p : "", key_p ? key_len : 0);
    /* Enter the TARGET's realm: a stub defined on a sub-realm global must be
     * that realm's function (its Function.prototype, its intrinsics). */
    JSAutoRealm ar(rt->cx, obj);
    JS::RootedObject fnObj(rt->cx, new_host_stub(rt->cx, key, name, nargs, 0, host_func_call));
    if (!fnObj) {
        return make_result(false, "", take_error(rt->cx));
    }
    JS::RootedValue fnVal(rt->cx, JS::ObjectValue(*fnObj));
    if (!JS_DefineProperty(rt->cx, obj, name.c_str(), fnVal, JSPROP_ENUMERATE)) {
        return make_result(false, "", take_error(rt->cx));
    }
    return make_result(true, "defined", "");
}

/* Define a CONSTRUCTABLE host function `name` on obj_handle — like
 * js_define_function, but `new name(...)` is allowed. host_func_call handles a
 * construct call the same way as a plain call (args in, one value out), so the
 * Go function's returned object becomes the instance (JS: a constructor that
 * returns an object yields that object). This is the primitive a real host
 * class (e.g. `new Worker(...)`) is built on. */
std::string js_define_constructor(uint64_t h, uint64_t obj_handle, const char *name_p,
                                  uint32_t name_len, const char *key_p, uint32_t key_len,
                                  uint32_t nargs) {
    Runtime *rt = rt_from(h);
    if (!rt || !rt->cx) {
        return "{\"ok\":false,\"result\":\"\",\"error\":\"no runtime\"}";
    }
    JS::RootedObject obj(rt->cx, obj_from_handle(obj_handle));
    if (!obj) {
        return make_result(false, "", "invalid object handle");
    }
    const std::string name(name_p ? name_p : "", name_p ? name_len : 0);
    const std::string key(key_p ? key_p : "", key_p ? key_len : 0);
    JSAutoRealm ar(rt->cx, obj);
    JS::RootedObject fnObj(
        rt->cx, new_host_stub(rt->cx, key, name, nargs, JSFUN_CONSTRUCTOR, host_func_call));
    if (!fnObj) {
        return make_result(false, "", take_error(rt->cx));
    }
    JS::RootedValue fnVal(rt->cx, JS::ObjectValue(*fnObj));
    if (!JS_DefineProperty(rt->cx, obj, name.c_str(), fnVal, JSPROP_ENUMERATE)) {
        return make_result(false, "", take_error(rt->cx));
    }
    return make_result(true, "defined", "");
}

void js_free_object(uint64_t obj_handle) {
    if (obj_handle) {
        delete reinterpret_cast<JS::PersistentRooted<JS::Value> *>(obj_handle);
    }
}

/* --- JS value <-> Go value codec --------------------------------------------
 * A JS value crosses to Go preserving IDENTITY: a primitive carries its data, an
 * object or function carries a GC-stable handle (a JS::PersistentRooted pointer)
 * so Go keeps the SAME object and can navigate and call it. A JS::Value is never
 * stringified across the bridge — that would drop the shared address. The
 * encoding is a small JSON tag {"k":kind,"v":primitive} or {"k":object,"h":handle}. */
static void encode_js_value(JSContext *cx, JS::HandleValue val, std::string &out) {
    if (val.isBoolean()) {
        out += val.toBoolean() ? "{\"k\":\"bool\",\"v\":true}" : "{\"k\":\"bool\",\"v\":false}";
        return;
    }
    if (val.isNumber()) {
        double d = val.toNumber();
        /* JSON has no NaN/Infinity literal; tag the specials as strings the
         * decoder maps back. %.17g would emit "nan"/"inf" — invalid JSON. */
        if (std::isnan(d)) {
            out += "{\"k\":\"number\",\"v\":\"NaN\"}";
            return;
        }
        if (std::isinf(d)) {
            out += d > 0 ? "{\"k\":\"number\",\"v\":\"Infinity\"}"
                         : "{\"k\":\"number\",\"v\":\"-Infinity\"}";
            return;
        }
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.17g", d);
        out += "{\"k\":\"number\",\"v\":";
        out += buf;
        out += "}";
        return;
    }
    if (val.isString()) {
        JS::RootedString s(cx, val.toString());
        std::string utf8 = jsstring_to_utf8(cx, s);
        out += "{\"k\":\"string\",\"v\":\"";
        json_escape(utf8, out);
        out += "\"}";
        return;
    }
    if (val.isNull()) {
        out += "{\"k\":\"null\"}";
        return;
    }
    if (val.isObject()) {
        JS::RootedObject o(cx, &val.toObject());
        uint64_t handle = new_obj_handle(cx, o);
        out += JS::IsCallable(o) ? "{\"k\":\"function\",\"h\":" : "{\"k\":\"object\",\"h\":";
        out += std::to_string(handle);
        out += "}";
        return;
    }
    /* Symbols and bigints have no host data representation, so — like objects —
     * they cross by handle, preserving identity (a symbol stays === itself). */
    if (val.isSymbol() || val.isBigInt()) {
        out += val.isSymbol() ? "{\"k\":\"symbol\",\"h\":" : "{\"k\":\"bigint\",\"h\":";
        out += std::to_string(new_val_handle(cx, val));
        out += "}";
        return;
    }
    out += "{\"k\":\"undefined\"}";
}

/* Parse a UTF-8 JSON string into a JS value via the realm's own JSON.parse. */
static bool parse_json_utf8(JSContext *cx, const char *data, size_t len, JS::MutableHandleValue out) {
    JS::RootedString s(cx, JS_NewStringCopyUTF8N(cx, JS::UTF8Chars(data, len)));
    if (!s) {
        return false;
    }
    JS::RootedObject global(cx, JS::CurrentGlobalOrNull(cx));
    JS::RootedValue jsonObjVal(cx);
    if (!global || !JS_GetProperty(cx, global, "JSON", &jsonObjVal) || !jsonObjVal.isObject()) {
        return false;
    }
    JS::RootedObject jsonObj(cx, &jsonObjVal.toObject());
    JS::RootedValue parseFn(cx);
    if (!JS_GetProperty(cx, jsonObj, "parse", &parseFn)) {
        return false;
    }
    JS::RootedValueArray<1> args(cx);
    args[0].setString(s);
    return JS::Call(cx, jsonObjVal, parseFn, args, out);
}

/* Decode one Go-side value encoding (a parsed {k,...} object) into a JS value. */
static void decode_js_value(JSContext *cx, JS::HandleValue encoded, JS::MutableHandleValue out) {
    out.setUndefined();
    if (!encoded.isObject()) {
        return;
    }
    JS::RootedObject obj(cx, &encoded.toObject());
    JS::RootedValue kVal(cx);
    if (!JS_GetProperty(cx, obj, "k", &kVal) || !kVal.isString()) {
        return;
    }
    JS::RootedString kStr(cx, kVal.toString());
    std::string k = jsstring_to_utf8(cx, kStr);
    if (k == "bool" || k == "number" || k == "string") {
        JS::RootedValue v(cx);
        if (JS_GetProperty(cx, obj, "v", &v)) {
            if (k == "number" && v.isString()) {
                /* The tagged non-finite specials (see encode_js_value). */
                JS::RootedString str(cx, v.toString());
                std::string sv = jsstring_to_utf8(cx, str);
                if (sv == "NaN") {
                    out.setNaN();
                } else if (sv == "Infinity") {
                    out.setDouble(std::numeric_limits<double>::infinity());
                } else if (sv == "-Infinity") {
                    out.setDouble(-std::numeric_limits<double>::infinity());
                }
                return;
            }
            out.set(v);
        }
        return;
    }
    if (k == "null") {
        out.setNull();
        return;
    }
    if (k == "object" || k == "function") {
        JS::RootedValue hVal(cx);
        if (JS_GetProperty(cx, obj, "h", &hVal) && hVal.isNumber()) {
            JSObject *o = obj_from_handle((uint64_t)hVal.toNumber());
            if (o) {
                out.setObject(*o);
            }
        }
        return;
    }
    if (k == "symbol" || k == "bigint") {
        JS::RootedValue hVal(cx);
        if (JS_GetProperty(cx, obj, "h", &hVal) && hVal.isNumber()) {
            out.set(val_from_handle((uint64_t)hVal.toNumber()));
        }
        return;
    }
    if (k == "json") {
        /* Host-side composite data (a Go slice/map/struct) materializing as a
         * FRESH guest Array/Object. The encoding reached us through JSON.parse
         * (parse_json_utf8), so "v" already IS the live value — no second
         * parse. Go-to-JS only: guest objects still cross as handles, since a
         * value with an identity must keep it. */
        JS::RootedValue v(cx);
        if (JS_GetProperty(cx, obj, "v", &v)) {
            out.set(v);
        }
        return;
    }
}

static std::string encode_error(JSContext *cx) {
    std::string out = "{\"k\":\"error\",\"v\":\"";
    json_escape(take_error(cx), out);
    out += "\"}";
    return out;
}

/* Get obj_handle[name] as a Go value encoding (primitive data, or an object /
 * function handle), preserving identity. */
std::string js_get(uint64_t h, uint64_t obj_handle, const char *name_p, uint32_t name_len) {
    Runtime *rt = rt_from(h);
    if (!rt || !rt->cx) {
        return "{\"k\":\"undefined\"}";
    }
    JS::RootedObject obj(rt->cx, obj_from_handle(obj_handle));
    if (!obj) {
        return "{\"k\":\"undefined\"}";
    }
    const std::string name(name_p ? name_p : "", name_p ? name_len : 0);
    JSAutoRealm ar(rt->cx, obj);
    JS::RootedValue val(rt->cx);
    if (!JS_GetProperty(rt->cx, obj, name.c_str(), &val)) {
        return encode_error(rt->cx);
    }
    std::string out;
    encode_js_value(rt->cx, val, out);
    return out;
}

/* Call the callable fn_handle with this_handle (0 = undefined) and args (a Go
 * JSON array of value encodings); return the result as a value encoding. */
/* Decode a JSON array of value encodings into live call arguments — shared by
 * js_call and js_construct. Returns "" on success, or the error encoding to
 * reply with. */
static std::string decode_call_args(JSContext *cx, const char *args_p, uint32_t args_len,
                                    JS::RootedValueVector &out) {
    if (args_len == 0) {
        return "";
    }
    JS::RootedValue argsArr(cx);
    if (!parse_json_utf8(cx, args_p, args_len, &argsArr) || !argsArr.isObject()) {
        return "{\"k\":\"error\",\"v\":\"undecodable call arguments\"}";
    }
    JS::RootedObject arr(cx, &argsArr.toObject());
    uint32_t len = 0;
    if (!JS::GetArrayLength(cx, arr, &len)) {
        return "{\"k\":\"error\",\"v\":\"undecodable call arguments\"}";
    }
    for (uint32_t i = 0; i < len; i++) {
        JS::RootedValue el(cx);
        if (!JS_GetElement(cx, arr, i, &el)) {
            return "{\"k\":\"error\",\"v\":\"undecodable call arguments\"}";
        }
        JS::RootedValue dec(cx);
        decode_js_value(cx, el, &dec);
        if (!out.append(dec)) {
            return "{\"k\":\"error\",\"v\":\"out of memory\"}";
        }
    }
    return "";
}

std::string js_call(uint64_t h, uint64_t fn_handle, uint64_t this_handle, const char *args_p,
                    uint32_t args_len) {
    Runtime *rt = rt_from(h);
    if (!rt || !rt->cx) {
        return "{\"k\":\"undefined\"}";
    }
    JS::RootedObject fnObj(rt->cx, obj_from_handle(fn_handle));
    if (!fnObj || !JS::IsCallable(fnObj)) {
        return "{\"k\":\"error\",\"v\":\"not a function\"}";
    }
    JSAutoRealm ar(rt->cx, fnObj);
    JS::RootedValue thisVal(rt->cx, JS::UndefinedValue());
    if (this_handle) {
        if (JSObject *t = obj_from_handle(this_handle)) {
            thisVal.setObject(*t);
        }
    }
    JS::RootedValueVector callArgs(rt->cx);
    if (std::string err = decode_call_args(rt->cx, args_p, args_len, callArgs); !err.empty()) {
        return err;
    }
    JS::RootedValue fnVal(rt->cx, JS::ObjectValue(*fnObj));
    JS::RootedValue rval(rt->cx);
    if (!JS::Call(rt->cx, thisVal, fnVal, callArgs, &rval)) {
        return encode_error(rt->cx);
    }
    std::string out;
    encode_js_value(rt->cx, rval, out);
    return out;
}

/* A fresh host-backed function object (see js.h): a js_define_function stub
 * returned as a handle instead of being defined as a property. */
uint64_t js_new_function(uint64_t h, const char *name_p, uint32_t name_len, const char *key_p,
                         uint32_t key_len, uint32_t nargs) {
    Runtime *rt = rt_from(h);
    if (!rt || !rt->cx) {
        return 0;
    }
    JS::RootedObject global(rt->cx, rt->global->get());
    JSAutoRealm ar(rt->cx, global);
    const std::string name(name_p ? name_p : "", name_p ? name_len : 0);
    const std::string key(key_p ? key_p : "", key_p ? key_len : 0);
    JS::RootedObject fnObj(rt->cx, new_host_stub(rt->cx, key, name, nargs, 0, host_func_call));
    if (!fnObj) {
        JS_ClearPendingException(rt->cx);
        return 0;
    }
    return new_obj_handle(rt->cx, fnObj);
}

/* Construct `new fn(...args)` — js_call's [[Construct]] counterpart. */
std::string js_construct(uint64_t h, uint64_t fn_handle, const char *args_p, uint32_t args_len) {
    Runtime *rt = rt_from(h);
    if (!rt || !rt->cx) {
        return "{\"k\":\"undefined\"}";
    }
    JS::RootedObject fnObj(rt->cx, obj_from_handle(fn_handle));
    if (!fnObj || !JS::IsConstructor(fnObj)) {
        return "{\"k\":\"error\",\"v\":\"not a constructor\"}";
    }
    JSAutoRealm ar(rt->cx, fnObj);
    JS::RootedValueVector callArgs(rt->cx);
    if (std::string err = decode_call_args(rt->cx, args_p, args_len, callArgs); !err.empty()) {
        return err;
    }
    JS::RootedValue fnVal(rt->cx, JS::ObjectValue(*fnObj));
    JS::RootedObject instance(rt->cx);
    if (!JS::Construct(rt->cx, fnVal, callArgs, &instance)) {
        return encode_error(rt->cx);
    }
    JS::RootedValue rval(rt->cx, JS::ObjectValue(*instance));
    std::string out;
    encode_js_value(rt->cx, rval, out);
    return out;
}

/* Set obj_handle[name] to the decoded value encoding `val` (a primitive, or an
 * object/function by handle). Returns {"k":"undefined"} on success or an error
 * encoding. */
std::string js_set(uint64_t h, uint64_t obj_handle, const char *name_p, uint32_t name_len,
                   const char *val_p, uint32_t val_len) {
    Runtime *rt = rt_from(h);
    if (!rt || !rt->cx) {
        return "{\"k\":\"error\",\"v\":\"no runtime\"}";
    }
    JS::RootedObject obj(rt->cx, obj_from_handle(obj_handle));
    if (!obj) {
        return "{\"k\":\"error\",\"v\":\"invalid object handle\"}";
    }
    const std::string name(name_p ? name_p : "", name_p ? name_len : 0);
    JSAutoRealm ar(rt->cx, obj);
    JS::RootedValue encoded(rt->cx);
    if (!parse_json_utf8(rt->cx, val_p, val_len, &encoded)) {
        JS_ClearPendingException(rt->cx);
        return "{\"k\":\"error\",\"v\":\"undecodable value\"}";
    }
    JS::RootedValue v(rt->cx);
    decode_js_value(rt->cx, encoded, &v);
    if (!JS_DefineProperty(rt->cx, obj, name.c_str(), v, JSPROP_ENUMERATE)) {
        return encode_error(rt->cx);
    }
    return "{\"k\":\"undefined\"}";
}

/* --- structured-clone handles ------------------------------------------------
 * A structured clone is how a value crosses agents (and how a SAB SHARES its
 * memory rather than copying). Serialization must run on the thread owning
 * the value's context, so a clone travels as a HANDLE — a heap
 * JSAutoStructuredCloneBuffer* the host owns: written on the sender's thread,
 * read on the receiver's, freed by the host (js_clone_free). */

/* SameProcess + shared-memory objects: a cloned SAB references the same
 * memory (that is what makes it shared); everything else is copied data.
 * The intra-cluster bit avoids the browser-flavoured COOP/COEP TypeError —
 * all agents here live in one process, one agent cluster by construction. */
static JS::CloneDataPolicy shared_clone_policy() {
    JS::CloneDataPolicy policy;
    policy.allowSharedMemoryObjects();
    policy.allowIntraClusterClonableSharedObjects();
    return policy;
}

/* Serializes reads and frees: the host may hand ONE clone (a broadcast) to
 * several agents, whose reads would otherwise race on the buffer. */
static std::mutex g_clone_mu;

static uint64_t clone_handle_write(JSContext *cx, JS::HandleValue v) {
    auto clone = std::make_unique<JSAutoStructuredCloneBuffer>(
        JS::StructuredCloneScope::SameProcess, nullptr, nullptr);
    if (!clone->write(cx, v, JS::UndefinedHandleValue, shared_clone_policy())) {
        return 0;
    }
    return reinterpret_cast<uint64_t>(clone.release());
}

static bool clone_handle_read(JSContext *cx, uint64_t handle, JS::MutableHandleValue out) {
    auto *clone = reinterpret_cast<JSAutoStructuredCloneBuffer *>(handle);
    if (!clone) {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_clone_mu);
    return clone->read(cx, out, shared_clone_policy(), nullptr, nullptr);
}

/* Host side: clone the decoded value encoding into a clone handle (0 = the
 * value is not clonable; the error is cleared). */
uint64_t js_clone_write(uint64_t h, const char *val_p, uint32_t val_len) {
    Runtime *rt = rt_from(h);
    if (!rt || !rt->cx) {
        return 0;
    }
    JS::RootedObject global(rt->cx, rt->global->get());
    JSAutoRealm ar(rt->cx, global);
    JS::RootedValue encoded(rt->cx);
    if (!parse_json_utf8(rt->cx, val_p, val_len, &encoded)) {
        JS_ClearPendingException(rt->cx);
        return 0;
    }
    JS::RootedValue v(rt->cx);
    decode_js_value(rt->cx, encoded, &v);
    uint64_t handle = clone_handle_write(rt->cx, v);
    if (!handle) {
        JS_ClearPendingException(rt->cx);
    }
    return handle;
}

/* Host side: deserialize a clone handle into the MAIN runtime and return the
 * value encoding. The handle stays valid (free it with js_clone_free). */
std::string js_clone_read(uint64_t h, uint64_t clone_handle) {
    Runtime *rt = rt_from(h);
    if (!rt || !rt->cx || clone_handle == 0) {
        return "{\"k\":\"error\",\"v\":\"invalid clone handle\"}";
    }
    JS::RootedObject global(rt->cx, rt->global->get());
    JSAutoRealm ar(rt->cx, global);
    JS::RootedValue v(rt->cx);
    if (!clone_handle_read(rt->cx, clone_handle, &v)) {
        return encode_error(rt->cx);
    }
    std::string out;
    encode_js_value(rt->cx, v, out);
    return out;
}

void js_clone_free(uint64_t clone_handle) {
    if (clone_handle) {
        std::lock_guard<std::mutex> lock(g_clone_mu);
        delete reinterpret_cast<JSAutoStructuredCloneBuffer *>(clone_handle);
    }
}

std::string js_eval_module(uint64_t h, const char *specifier_p, uint32_t specifier_len,
                           const char *src_p, uint32_t src_len) {
    const std::string specifier(specifier_p ? specifier_p : "", specifier_p ? specifier_len : 0);
    const std::string src(src_p ? src_p : "", src_p ? src_len : 0);
    Runtime *rt = rt_from(h);
    if (!rt || !rt->cx) {
        return "{\"ok\":false,\"result\":\"\",\"error\":\"no runtime\"}";
    }
    JS::RootedObject global(rt->cx, rt->global->get());
    JSAutoRealm ar(rt->cx, global);
    if (rt->bits_addr == nullptr) {
        JS_RequestInterruptCallback(rt->cx);
    }

    JS::RootedObject module(rt->cx, compile_and_register_module(rt->cx, specifier, src));
    if (!module) {
        return make_result(false, "", take_error(rt->cx));
    }
    JS::RootedValue hd(rt->cx, JS::ObjectValue(*module));
    if (!JS::LoadRequestedModules(rt->cx, module, hd, load_module_resolved,
                                  load_module_rejected) ||
        JS_IsExceptionPending(rt->cx)) {
        return make_result(false, "", take_error(rt->cx));
    }
    if (!JS::ModuleLink(rt->cx, module)) {
        return make_result(false, "", take_error(rt->cx));
    }
    JS::RootedValue rval(rt->cx);
    if (!JS::ModuleEvaluate(rt->cx, module, &rval)) {
        return make_result(false, "", take_error(rt->cx));
    }
    js::RunJobs(rt->cx);
    if (JS_IsExceptionPending(rt->cx)) {
        return make_result(false, "", take_error(rt->cx));
    }

    /* With top-level await the result is the evaluation promise; report by its
     * settled state. No timers exist, so a still-pending promise can never
     * settle: that is an error, not something to wait on. */
    if (rval.isObject()) {
        JS::RootedObject promise(rt->cx, &rval.toObject());
        if (JS::IsPromiseObject(promise)) {
            switch (JS::GetPromiseState(promise)) {
            case JS::PromiseState::Fulfilled:
                return make_result(true, "undefined", "");
            case JS::PromiseState::Rejected: {
                JS::RootedValue reason(rt->cx, JS::GetPromiseResult(promise));
                JS_SetPendingException(rt->cx, reason);
                return make_result(false, "", take_error(rt->cx));
            }
            case JS::PromiseState::Pending:
                return make_result(false, "",
                                   "module evaluation did not settle "
                                   "(top-level await on something that never resolves)");
            }
        }
    }
    return make_result(true, "undefined", "");
}

/* ---- realm / engine primitives ---------------------------------------------
 * Generic JSAPI bindings a conformance harness (or any embedder) composes
 * host-side. Nothing test262-shaped lives in C++: $262 is assembled in Go
 * from these plus the object/value bindings above. */

/* Force a full garbage collection. */
void js_gc(uint64_t h) {
    Runtime *rt = rt_from(h);
    if (!rt || !rt->cx) {
        return;
    }
    JS_GC(rt->cx);
}

/* Detach an ArrayBuffer (JS::DetachArrayBuffer). Returns {"k":"undefined"} or
 * an error encoding. */
std::string js_detach_array_buffer(uint64_t h, uint64_t obj_handle) {
    Runtime *rt = rt_from(h);
    if (!rt || !rt->cx) {
        return "{\"k\":\"error\",\"v\":\"no runtime\"}";
    }
    JS::RootedObject obj(rt->cx, obj_from_handle(obj_handle));
    if (!obj) {
        return "{\"k\":\"error\",\"v\":\"invalid object handle\"}";
    }
    JSAutoRealm ar(rt->cx, rt->global->get());
    if (!JS::DetachArrayBuffer(rt->cx, obj)) {
        return encode_error(rt->cx);
    }
    return "{\"k\":\"undefined\"}";
}

/* Create a fresh Uint8Array holding a copy of data. The copy happens entirely
 * inside this call — JS_GetUint8ArrayData's pointer is used under
 * AutoCheckCannotGC and never crosses the bridge — so inline (GC-movable)
 * array data is safe. */
uint64_t js_bytes_new(uint64_t h, const char *data, uint32_t data_len) {
    Runtime *rt = rt_from(h);
    if (!rt || !rt->cx) {
        return 0;
    }
    JS::RootedObject global(rt->cx, rt->global->get());
    JSAutoRealm ar(rt->cx, global);
    JS::RootedObject arr(rt->cx, JS_NewUint8Array(rt->cx, data_len));
    if (!arr) {
        JS_ClearPendingException(rt->cx);
        return 0;
    }
    if (data_len > 0) {
        JS::AutoCheckCannotGC nogc;
        bool is_shared = false;
        uint8_t *dst = JS_GetUint8ArrayData(arr, &is_shared, nogc);
        if (!dst) {
            return 0;
        }
        memcpy(dst, data, data_len);
    }
    return new_obj_handle(rt->cx, arr);
}

/* Copy the binary contents of obj_handle into the reply ('B' + bytes, or 'E' +
 * message). The engine-side data pointer is only dereferenced here, before any
 * further JSAPI call, so GC cannot move the data out from under the copy. */
std::string js_bytes_read(uint64_t h, uint64_t obj_handle) {
    Runtime *rt = rt_from(h);
    if (!rt || !rt->cx) {
        return "Eno runtime";
    }
    JSObject *obj = obj_from_handle(obj_handle);
    if (!obj) {
        return "Einvalid object handle";
    }
    size_t length = 0;
    bool is_shared = false;
    uint8_t *data = nullptr;
    JS::AutoCheckCannotGC nogc;
    if (JS_GetObjectAsArrayBufferView(obj, &length, &is_shared, &data)) {
        /* Any view — Uint8Array, other typed arrays, DataView — read as its
         * raw byte window (offset/length already applied). */
    } else if (JS::IsArrayBufferObject(obj)) {
        length = JS::GetArrayBufferByteLength(obj);
        data = JS::GetArrayBufferData(obj, &is_shared, nogc);
    } else if (JS::IsSharedArrayBufferObject(obj)) {
        length = JS::GetSharedArrayBufferByteLength(obj);
        data = JS::GetSharedArrayBufferData(obj, &is_shared, nogc);
    } else {
        return "Evalue is not an ArrayBuffer or ArrayBuffer view";
    }
    std::string out;
    out.reserve(length + 1);
    out += 'B';
    if (data && length > 0) {
        out.append(reinterpret_cast<const char *>(data), length);
    }
    return out;
}

/* Create a fresh SAME-COMPARTMENT realm (objects flow between realms directly,
 * no cross-compartment wrappers) with the standard classes, and return its
 * global object as a handle. The realm's host surface is EMPTY — the embedder
 * defines what it wants on the returned global (js_define_function/js_set
 * enter the target object's realm). */
uint64_t js_new_realm(uint64_t h) {
    Runtime *rt = rt_from(h);
    if (!rt || !rt->cx) {
        return 0;
    }
    static JSClass realm_global_class = {"global", JSCLASS_GLOBAL_FLAGS,
                                         &JS::DefaultGlobalClassOps};
    JSAutoRealm ar(rt->cx, rt->global->get());
    JS::RealmOptions options;
    JS::RootedObject current(rt->cx, JS::CurrentGlobalOrNull(rt->cx));
    options.creationOptions().setExistingCompartment(current);
    options.creationOptions().setSharedMemoryAndAtomicsEnabled(true);
    JS::RootedObject newGlobal(rt->cx, JS_NewGlobalObject(rt->cx, &realm_global_class, nullptr,
                                                          JS::FireOnNewGlobalHook, options));
    if (!newGlobal) {
        JS_ClearPendingException(rt->cx);
        return 0;
    }
    {
        JSAutoRealm enter(rt->cx, newGlobal);
        if (!JS::InitRealmStandardClasses(rt->cx)) {
            JS_ClearPendingException(rt->cx);
            return 0;
        }
    }
    return new_obj_handle(rt->cx, newGlobal);
}

/* Evaluate src as a classic script in the realm of `global_handle` (any
 * global from js_new_realm, or js_global for the main realm) and return the
 * completion value as a value encoding — {"k":"error",...} when it threw.
 * Deliberately does NOT drain the job queue: this is the raw synchronous
 * evaluation primitive; job policy stays with the caller. */
std::string js_eval_in(uint64_t h, uint64_t global_handle, const char *src_p,
                       uint32_t src_len) {
    Runtime *rt = rt_from(h);
    if (!rt || !rt->cx) {
        return "{\"k\":\"error\",\"v\":\"no runtime\"}";
    }
    JS::RootedObject target(rt->cx, obj_from_handle(global_handle));
    if (!target) {
        return "{\"k\":\"error\",\"v\":\"invalid global handle\"}";
    }
    JSAutoRealm ar(rt->cx, target);
    JS::CompileOptions opts(rt->cx);
    opts.setFileAndLine("<eval_in>", 1);
    JS::SourceText<mozilla::Utf8Unit> buf;
    if (!buf.init(rt->cx, src_p ? src_p : "", src_p ? src_len : 0,
                  JS::SourceOwnership::Borrowed)) {
        JS_ClearPendingException(rt->cx);
        return "{\"k\":\"error\",\"v\":\"could not read source\"}";
    }
    JS::RootedValue rval(rt->cx);
    if (!JS::Evaluate(rt->cx, opts, buf, &rval)) {
        /* Carry the exception VALUE out (with its type intact), tagged thrown,
         * so the caller can re-throw it — evalScript must propagate a
         * SyntaxError as a SyntaxError, not a stringified message. */
        JS::RootedValue exc(rt->cx);
        if (!JS_GetPendingException(rt->cx, &exc)) {
            return encode_error(rt->cx); /* uncatchable termination */
        }
        JS_ClearPendingException(rt->cx);
        std::string out;
        encode_js_value(rt->cx, exc, out);
        /* encode_js_value always ends with '}'; splice the thrown marker in. */
        out.insert(out.size() - 1, ",\"thrown\":true");
        return out;
    }
    std::string out;
    encode_js_value(rt->cx, rval, out);
    return out;
}

/* [[IsHTMLDDA]]: an object that emulates undefined and yields null when
 * called (document.all semantics). The class flag is engine-level, so the
 * OBJECT is the primitive; what a harness does with it is host policy. */
static bool is_htmldda_call(JSContext *cx, unsigned argc, JS::Value *vp) {
    (void)cx;
    JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
    args.rval().setNull();
    return true;
}

static const JSClassOps is_htmldda_ops = {
    .call = is_htmldda_call,
};

static const JSClass is_htmldda_class = {
    "IsHTMLDDA",
    JSCLASS_EMULATES_UNDEFINED,
    &is_htmldda_ops,
};

/* A fresh [[IsHTMLDDA]] object, as a handle. */
uint64_t js_new_htmldda(uint64_t h) {
    Runtime *rt = rt_from(h);
    if (!rt || !rt->cx) {
        return 0;
    }
    JSAutoRealm ar(rt->cx, rt->global->get());
    return new_obj_handle(rt->cx, JS_NewObject(rt->cx, &is_htmldda_class));
}

/* ---- agents (ECMA-262 agents; threads builds only) -------------------------
 *
 * ECMA-262 specifies what an agent IS (its own thread of execution, its own
 * realm, sharing nothing with other agents but SharedArrayBuffer memory — one
 * agent cluster) and leaves creation and communication to the host. This
 * bridge keeps ONLY what physically cannot leave C++:
 *
 *   - the thread mechanics: a JSContext is single-threaded, so creating an
 *     agent's context/global, evaluating its source and pumping its job queue
 *     must run ON the agent's thread (js_agent_spawn + agent_thread_main);
 *   - structured clone write/read, which must run on the thread owning the
 *     value's context (js_clone_write/read/free for the host side; the agent
 *     side clones directly on its own context).
 *
 * EVERYTHING ELSE — queues, broadcast latching, routing, per-agent channels,
 * lifecycle tracking — is host policy and lives in Go: the agent-side
 * primitives (__agent__.receive/post and the exit notification) are forwarded
 * to the host through reserved go_host_call keys, exactly like the module
 * loader. An agent's receive() simply blocks its goroutine inside the host
 * until the host hands back a clone handle. $262.agent, Web Workers and Node
 * worker_threads are all adapters the embedder composes host-side (plus a
 * guest-side prelude prepended to the agent source).
 *
 * Values cross agents as STRUCTURED CLONES with shared-memory objects allowed
 * — the spec route: a cloned SharedArrayBuffer shares the SAME memory,
 * everything else is deep-copied data. A clone travels between threads as a
 * CLONE HANDLE (a heap JSAutoStructuredCloneBuffer*), owned by the host.
 *
 * A thread here is a pthread, which wasi-libc turns into a wasi_thread_spawn —
 * which wasm2go runs on a GOROUTINE. So the whole chain is: guest JS agent ->
 * SpiderMonkey thread -> pthread -> wasi_thread_spawn -> goroutine.
 *
 * Only compiled when the engine was built for wasi-threads
 * (scripts/build-engine-intl.sh with SPIDERMONKEY_THREADS=1); the
 * single-agent build stubs js_agent_spawn (returns 0). */
#ifdef SPIDERMONKEY_WASM_THREADS

#include <pthread.h>

#include <algorithm>
#include <deque>
#include <mutex>

namespace js {
/* Added by the engine patch in scripts/build-engine-intl.sh (threads builds):
 * milliseconds until the earliest engine-delayed dispatchable (an
 * Atomics.waitAsync timeout) is due — 0 if one is already due, -1 if none.
 * The agent pump bounds its idle futex wait by this, since such tasks are
 * invisible to RunJobs until dispatched. */
extern int64_t Wasm2GoEarliestDelayedDispatchMs(JSContext *cx);
/* Also from the engine patch: fires the given callback after EVERY internal
 * dispatch-queue append (an Atomics.notify resolving a waitAsync in another
 * runtime arrives that way). Registered once in js_new so a parked agent
 * pump hears cross-thread work; the callback runs under the engine's helper
 * lock and must not take locks. */
extern void SetWasm2GoDispatchWakeup(void (*fn)());
}

/* The process-wide agent WAKE futex (AgentState — the per-runtime lifecycle
 * state — lives in Runtime::agents). A pure wake channel, like a condition
 * variable: leaving, shutdown and the engine's dispatch-wakeup callback bump
 * and notify it, and every waiter re-checks its own runtime's conditions
 * after a wake, so waking every parked agent in the process is safe. It has
 * to be process-wide: SetWasm2GoDispatchWakeup's callback carries no runtime
 * context. */
static std::atomic<uint32_t> g_agent_epoch{0};

/* Bump the event epoch and wake every agent parked on it. */
static void agent_event_notify_all() {
    g_agent_epoch.fetch_add(1, std::memory_order_seq_cst);
    __builtin_wasm_memory_atomic_notify((int *)&g_agent_epoch, INT32_MAX);
}

/* Park until the epoch moves past `seen` (or the timeout, in ns, expires;
 * negative = forever). The futex compare-and-park makes this race-free:
 * an epoch bump between load and wait returns immediately. */
static void agent_event_wait(uint32_t seen, int64_t timeout_ns) {
    __builtin_wasm_memory_atomic_wait32((int *)&g_agent_epoch, (int32_t)seen, timeout_ns);
}

/* Agent diagnostics go to STDERR, never to the $262.agent report queue:
 * test262 compares reports BY VALUE, so a diagnostic in the queue silently
 * corrupts the assertion under test. */
static void agent_milestone(const char *what) {
    fprintf(stderr, "[agent] %s\n", what);
}


struct AgentStart {
    std::string glue; /* trusted adapter setup — evaluated as its OWN script */
    std::string src;  /* the user's source — evaluated as a SEPARATE script, so
                       * its "use strict", line numbers and directives are its
                       * own (never shifted by a prepended prelude) */
    uint64_t id = 0;
    Runtime *rt = nullptr; /* the runtime this agent's cluster belongs to */
};

/* Agent-side host communication rides reserved go_host_call keys — a NUL
 * prefix plus an "agent-" op name, so they can never collide with a host
 * function name (exactly like the module loader's key). The HOST owns all
 * policy behind them; the C++ side only forwards. The one key C++ composes
 * itself is the exit notification the thread skeleton sends and the
 * inbox drain the pump runs. */
static const std::string kAgentExitKey("\0agent-exit", 11);
static const std::string kAgentTryInboxKey("\0agent-try-inbox", 16);

/* One reserved-key round trip to the host from THIS (agent) thread. Returns
 * false when no host is attached (total == 0). tag/payload are the reply. */
static bool agent_host_call(const std::string &key, const std::string &args, char *tag,
                            std::string *payload) {
    std::vector<char> out(256);
    uint32_t total = go_host_call(key.data(), (uint32_t)key.size(), args.data(),
                                  (uint32_t)args.size(), 0, out.data(), (uint32_t)out.size());
    if (total == 0) {
        return false;
    }
    if (total > out.size()) {
        out.resize(total);
        go_host_result(out.data());
    }
    *tag = out[0];
    payload->assign(out.data() + 1, total - 1);
    return true;
}

/* The agent global's NATIVE surface — the minimum that physically cannot
 * leave this thread, with no policy baked in. The embedder's prelude
 * (prepended to the agent source host-side) composes whatever API it wants
 * ($262.agent, a Worker scope, ...) from these and then deletes them from
 * the global, so the guest script never sees the raw channels. */

/* __agent_call__(op, extra?): one reserved-key round trip to the host from
 * THIS agent's thread. op is the bare channel name and MUST start with
 * "agent-" (the native prepends the NUL byte and injects this agent's id as
 * the first argument, so guest code can neither forge another reserved
 * channel nor impersonate another agent). extra, when present, is a number
 * (a clone handle, a millisecond count) appended as the second argument.
 * The host may BLOCK this goroutine as long as it wants (that is how
 * receive waits). Returns the raw reply — tag byte ('R'/'E') + payload —
 * as a string for the prelude to interpret. */
static bool agent_call_native(JSContext *cx, unsigned argc, JS::Value *vp) {
    JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
    JS::RootedString opStr(cx, JS::ToString(cx, args.get(0)));
    if (!opStr) {
        return false;
    }
    std::string op = jsstring_to_utf8(cx, opStr);
    if (op.rfind("agent-", 0) != 0) {
        JS_ReportErrorASCII(cx, "__agent_call__: unknown channel");
        return false;
    }
    std::string key("\0", 1);
    key += op;
    std::string callArgs = "[" + std::to_string(ctx_data(cx)->agent_id);
    if (args.hasDefined(1)) {
        double extra = 0;
        if (!JS::ToNumber(cx, args.get(1), &extra)) {
            return false;
        }
        callArgs += "," + std::to_string((uint64_t)extra);
    }
    callArgs += "]";
    char tag = 0;
    std::string payload;
    if (!agent_host_call(key, callArgs, &tag, &payload)) {
        JS_ReportErrorASCII(cx, "__agent_call__: no host attached");
        return false;
    }
    std::string reply(1, tag);
    reply += payload;
    JS::RootedString out(cx, JS_NewStringCopyN(cx, reply.data(), reply.size()));
    if (!out) {
        return false;
    }
    args.rval().setString(out);
    return true;
}

/* __clone_read__(handle): deserialize a host-owned clone into THIS agent's
 * runtime (a cloned SAB wraps the SAME memory; everything else is copied). */
static bool agent_clone_read_native(JSContext *cx, unsigned argc, JS::Value *vp) {
    JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
    double handle = 0;
    if (!JS::ToNumber(cx, args.get(0), &handle)) {
        return false;
    }
    JS::RootedValue val(cx);
    if (!clone_handle_read(cx, (uint64_t)handle, &val)) {
        JS_ReportErrorASCII(cx, "__clone_read__: invalid clone handle");
        return false;
    }
    args.rval().set(val);
    return true;
}

/* __clone_write__(v): structured-clone v on THIS thread; returns the clone
 * handle (the host takes ownership when the prelude posts it). */
static bool agent_clone_write_native(JSContext *cx, unsigned argc, JS::Value *vp) {
    JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
    uint64_t handle = clone_handle_write(cx, args.get(0));
    if (!handle) {
        return false;
    }
    args.rval().setNumber((double)handle);
    return true;
}

/* __agent_leaving__(): thread-loop control — the one signal that must stay
 * native, because the pump below checks the flag between drains. */
static bool agent_leaving_native(JSContext *cx, unsigned argc, JS::Value *vp) {
    JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
    ctx_data(cx)->agent_left = true;
    agent_event_notify_all();
    args.rval().setUndefined();
    return true;
}

static bool install_agent_natives(JSContext *cx, JS::HandleObject global) {
    return JS_DefineFunction(cx, global, "__agent_call__", agent_call_native, 2, 0) &&
           JS_DefineFunction(cx, global, "__clone_read__", agent_clone_read_native, 1, 0) &&
           JS_DefineFunction(cx, global, "__clone_write__", agent_clone_write_native, 1, 0) &&
           JS_DefineFunction(cx, global, "__agent_leaving__", agent_leaving_native, 0, 0);
}

/* Drain this agent's host inbox and hand each message to globalThis.__deliver__
 * (the adapter's glue defines it). Delivery runs ON the agent thread, right
 * before RunJobs, so a Worker's onmessage runs here and any async work it
 * queues drains in the RunJobs that follows — no JS Atomics.waitAsync polling
 * loop needed on the agent side (that both churns the engine and was a
 * needless clone leak). The pump owns each clone: it reads it on this cx and
 * frees the buffer. */
static void agent_deliver_inbox(JSContext *cx, uint64_t id, JS::HandleObject global) {
    JS::RootedValue deliver(cx);
    if (!JS_GetProperty(cx, global, "__deliver__", &deliver) || !deliver.isObject() ||
        !JS::IsCallable(&deliver.toObject())) {
        return; /* no adapter delivery hook: nothing to do */
    }
    for (;;) {
        char tag = 0;
        std::string payload;
        std::string args = "[" + std::to_string(id) + "]";
        if (!agent_host_call(kAgentTryInboxKey, args, &tag, &payload)) {
            return; /* no host attached */
        }
        if (tag != 'R' || payload.empty()) {
            return; /* 'E' shutdown, or the inbox is empty */
        }
        uint64_t handle = strtoull(payload.c_str(), nullptr, 10);
        JS::RootedValue val(cx);
        if (clone_handle_read(cx, handle, &val)) {
            JS::RootedValueArray<1> cbArgs(cx);
            cbArgs[0].set(val);
            JS::RootedValue rval(cx);
            if (!JS_CallFunctionValue(cx, nullptr, deliver, cbArgs, &rval)) {
                JS_ClearPendingException(cx); /* a handler throw must not kill the pump */
            }
        }
        js_clone_free(handle); /* the pump owns the clone once dequeued */
    }
}

/* One agent = one thread = one runtime. */
static void *agent_thread_main(void *arg) {
    std::unique_ptr<AgentStart> start(static_cast<AgentStart *>(arg));

    Runtime *rt = start->rt;

    /* This context's private data: its id for the reserved host calls and its
     * print capture (which dies with it). */
    CtxData ctxdata;
    ctxdata.rt = rt;
    ctxdata.agent_id = start->id;

    /* Parented to the main runtime — same agent cluster, which is what makes
     * the SharedArrayBuffer waiter list and the off-thread promise bookkeeping
     * (both cluster-scoped) reach this agent. */
    JSContext *cx = JS_NewContext(JS::DefaultHeapMaxBytes, rt->jsrt);
    if (!cx) {
        agent_milestone("JS_NewContext FAILED");
        return nullptr;
    }
    JS_SetContextPrivate(cx, &ctxdata);
    /* Blocking in Atomics.wait is what an agent is FOR; without this the
     * engine throws "waiting is not allowed on this thread". */
    JS_SetFutexCanWait(cx);
    /* Interrupt callbacks are PER CONTEXT: without one here, js_close's
     * urgent interrupt wakes an agent parked in Atomics.wait but nothing
     * terminates it — the wait just resumes for its remaining timeout, and
     * an infinite wait would make close hang forever. */
    if (!JS_AddInterruptCallback(cx, interrupt_cb)) {
        agent_milestone("interrupt callback FAILED");
        JS_DestroyContext(cx);
        return nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(rt->agents.mu);
        rt->agents.contexts.push_back(cx);
    }
    if (!js::UseInternalJobQueues(cx) || !JS::InitSelfHostedCode(cx)) {
        agent_milestone("selfhosted FAILED");
        JS_DestroyContext(cx);
        return nullptr;
    }
    static JSClass agent_global_class = {"global", JSCLASS_GLOBAL_FLAGS,
                                        &JS::DefaultGlobalClassOps};
    JS::RealmOptions options;
    options.creationOptions().setSharedMemoryAndAtomicsEnabled(true);
    JS::RootedObject global(
        cx, JS_NewGlobalObject(cx, &agent_global_class, nullptr, JS::FireOnNewGlobalHook, options));
    if (!global) {
        agent_milestone("global FAILED");
        JS_DestroyContext(cx);
        return nullptr;
    }
    {
        JSAutoRealm ar(cx, global);
        if (!JS::InitRealmStandardClasses(cx) || !install_agent_natives(cx, global)) {
            agent_milestone("realm FAILED");
            JS_DestroyContext(cx);
            return nullptr;
        }
        /* Evaluate the adapter GLUE and the user SOURCE as SEPARATE scripts.
         * Concatenating them (the old prelude approach) silently broke the
         * user source: a leading "use strict" stops being the first statement,
         * error line numbers shift by the glue's length, and a module source
         * cannot be spliced after a classic prelude. Each script here starts at
         * line 1 with its own directive prologue. */
        auto eval_script = [&](const std::string &code, const char *file) -> bool {
            JS::CompileOptions opts(cx);
            opts.setFileAndLine(file, 1);
            JS::SourceText<mozilla::Utf8Unit> buf;
            if (!buf.init(cx, code.data(), code.size(), JS::SourceOwnership::Borrowed)) {
                return false;
            }
            JS::RootedValue rval(cx);
            return JS::Evaluate(cx, opts, buf, &rval);
        };
        {
            bool ok = eval_script(start->glue, "<agent-glue>") &&
                      eval_script(start->src, "<agent>");
            if (ok) {
                /* The agent's work usually CONTINUES past evaluation (async
                 * receiveBroadcast callbacks). Deterministic loop, no polls:
                 *  - js::RunJobs itself BLOCKS (internal condvar) while the
                 *    engine holds outstanding off-thread promise work — a
                 *    pending waitAsync parks here until notify or timeout.
                 *  - Otherwise the agent parks on the event futex, bounded by
                 *    the next DEADLINE it must act on: the earliest host
                 *    timer, or the earliest engine-delayed dispatchable (an
                 *    Atomics.waitAsync timeout — invisible to RunJobs'
                 *    hasPending, so parking unbounded would strand it).
                 *    broadcast, leaving and shutdown bump-and-notify the
                 *    futex. */
                while (!ctxdata.agent_left &&
                       !rt->agents.shutdown.load(std::memory_order_seq_cst)) {
                    /* Read the wake epoch BEFORE checking the inbox: a Send that
                     * arrives after the drain below but before the park bumps
                     * the epoch past `seen`, so the park returns at once and the
                     * next iteration delivers it — no message is ever stranded
                     * (the standard futex compare-and-park discipline). */
                    uint32_t seen = g_agent_epoch.load(std::memory_order_seq_cst);
                    agent_deliver_inbox(cx, start->id, global);
                    js::RunJobs(cx);
                    JS_ClearPendingException(cx);
                    if (ctxdata.agent_left ||
                        rt->agents.shutdown.load(std::memory_order_seq_cst)) {
                        break;
                    }
                    /* Park bounded by the earliest engine-delayed dispatchable
                     * (an Atomics.waitAsync timeout — invisible to RunJobs'
                     * hasPending, so parking unbounded would strand it);
                     * woken early by Send (AgentWake), broadcast, leaving and
                     * shutdown. */
                    int64_t timeout_ns = -1;
                    int64_t delayed_ms = js::Wasm2GoEarliestDelayedDispatchMs(cx);
                    if (delayed_ms >= 0) {
                        timeout_ns = delayed_ms * 1000000;
                    }
                    agent_event_wait(seen, timeout_ns);
                }
            } else {
                /* Surface the pending exception's message — "evaluate FAILED"
                 * alone names no cause. */
                std::string msg = "evaluate FAILED: ";
                JS::RootedValue exc(cx);
                if (JS_GetPendingException(cx, &exc)) {
                    JS_ClearPendingException(cx);
                    JS::RootedString str(cx, JS::ToString(cx, exc));
                    if (str) {
                        JS::UniqueChars utf8 = JS_EncodeStringToUTF8(cx, str);
                        if (utf8) {
                            msg += utf8.get();
                        }
                    }
                }
                agent_milestone(msg.c_str());
            }
        }
        JS_ClearPendingException(cx);
    }
    {
        std::lock_guard<std::mutex> lock(rt->agents.mu);
        auto &v = rt->agents.contexts;
        v.erase(std::remove(v.begin(), v.end(), cx), v.end());
    }
    JS_DestroyContext(cx);
    /* Tell the host this agent is gone (fire-and-forget; the host tracks
     * lifecycle — its Alive count, releasing per-agent state). */
    {
        char tag = 0;
        std::string payload;
        std::string callArgs = "[" + std::to_string(start->id) + "]";
        agent_host_call(kAgentExitKey, callArgs, &tag, &payload);
    }
    /* Last: js_close's shutdown loop parks on the event futex until alive
     * hits zero; the notify is what releases it. */
    rt->agents.alive.fetch_sub(1, std::memory_order_seq_cst);
    agent_event_notify_all();
    return nullptr;
}

/* Host side: spawn a new agent. `glue` is trusted adapter setup and `src` is
 * the user source; they are evaluated as SEPARATE scripts on the agent (so the
 * user source keeps its own strict directive, line numbers, and can even be a
 * module) — never concatenated. Returns the agent's id (0 on failure). */
uint64_t js_agent_spawn(uint64_t h, const char *glue_p, uint32_t glue_len, const char *src_p,
                        uint32_t src_len) {
    Runtime *rt = rt_from(h);
    if (!rt || !rt->cx) {
        return 0;
    }
    uint64_t id = rt->agents.next_id.fetch_add(1, std::memory_order_relaxed);
    auto *start = new AgentStart{std::string(glue_p ? glue_p : "", glue_p ? glue_len : 0),
                                 std::string(src_p ? src_p : "", src_p ? src_len : 0), id, rt};

    pthread_t tid;
    /* Incremented BEFORE create: the agent may run to completion (and
     * decrement) before pthread_create even returns here. */
    rt->agents.alive.fetch_add(1, std::memory_order_seq_cst);
    if (pthread_create(&tid, nullptr, agent_thread_main, start) != 0) {
        rt->agents.alive.fetch_sub(1, std::memory_order_seq_cst);
        delete start;
        return 0;
    }
    /* NOT detached: js_close joins every agent so none can outlive the
     * runtime it shares an agent cluster with. */
    {
        std::lock_guard<std::mutex> lock(rt->agents.mu);
        rt->agents.threads.push_back(tid);
    }
    return id;
}

/* Wake every agent pump parked on the event futex. The host calls this after
 * delivering to an agent's inbox (Send), so the agent's pump re-checks its
 * inbox promptly instead of sleeping until its next engine deadline. Waking
 * all is safe: each pump re-checks its own state and re-parks. */
void js_agent_wake(uint64_t h) {
    (void)h;
    agent_event_notify_all();
}

#else /* !SPIDERMONKEY_WASM_THREADS: single-agent build — spawn fails. */

uint64_t js_agent_spawn(uint64_t h, const char *glue_p, uint32_t glue_len, const char *src_p,
                        uint32_t src_len) {
    (void)h;
    (void)glue_p;
    (void)glue_len;
    (void)src_p;
    (void)src_len;
    return 0;
}

void js_agent_wake(uint64_t h) { (void)h; }

#endif /* SPIDERMONKEY_WASM_THREADS */

/* ---- public API (js.h) --------------------------------------------------- */

uint64_t js_new(uint32_t max_heap_bytes, uint32_t native_stack_quota_bytes) {
    static bool inited = false;
    if (!inited) {
#ifdef SPIDERMONKEY_WASM_THREADS
        /* MUST precede JS_Init: the engine picks its internal thread pool
         * from whether a dispatch callback is already set. */
        JS::SetHelperThreadTaskCallback(helper_thread_dispatch, 8, 512 * 1024);
#endif
        if (!JS_Init()) {
            return 0;
        }
        inited = true;
    }

    auto *rt = new Runtime();

    /* The nursery is carved out of the heap budget, so a tiny max_heap_bytes
     * with SpiderMonkey's default nursery would leave nothing for the tenured
     * heap. Let SpiderMonkey size it; we only clamp the total.
     *
     * max_heap_bytes == 0 means UNCAPPED on the JS side (0xffffffff, not
     * JS::DefaultHeapMaxBytes — that default is a mere 32 MiB and starved
     * GC-heavy guests long before the real sandbox limit): the single
     * effective memory bound is then the wasm linear-memory cap the host
     * configures (go-spidermonkey Config.MaxMemoryBytes). One knob, host
     * side, by design. */
    rt->cx = JS_NewContext(max_heap_bytes ? max_heap_bytes : 0xffffffffu);
    if (!rt->cx) {
        delete rt;
        return 0;
    }
    /* Before anything can call back (interrupt callback, natives): the
     * context's private is how they find this runtime. */
    rt->main_ctx.rt = rt;
    JS_SetContextPrivate(rt->cx, &rt->main_ctx);

    /* The engine is built --disable-jit --enable-portable-baseline-interp,
     * but PBL is OFF by default at runtime: without this, every script runs
     * in the generic C++ interpreter (js::Interpret), the slowest tier.
     * Enable PBL and enter it immediately (warm-up 0) — with no JIT tiers
     * above it there is nothing to warm up FOR, and the generic interpreter
     * is strictly slower. Options are process-global; set once, before
     * InitSelfHostedCode so self-hosted code runs under PBL too. */
    JS_SetGlobalJitCompilerOption(rt->cx, JSJITCOMPILER_PORTABLE_BASELINE_ENABLE, 1);
    JS_SetGlobalJitCompilerOption(rt->cx, JSJITCOMPILER_PORTABLE_BASELINE_WARMUP_THRESHOLD, 0);

    /* Ordering below is not stylistic. Everything here has to happen before
     * JS::InitSelfHostedCode, which is the point after which the runtime counts
     * as started:
     *
     *   - js::UseInternalJobQueues hard-asserts on it
     *     (MOZ_RELEASE_ASSERT(!hasInitializedSelfHosting()), JSContext.cpp) —
     *     a MOZ_RELEASE_ASSERT fires in release builds too, so calling it late
     *     is not a warning, it is a wasm `unreachable` trap.
     *   - JS_SetNativeStackQuota is documented as callable "immediately after
     *     the runtime is initialized and before any code is executed", and
     *     InitSelfHostedCode executes self-hosted code.
     */
    if (max_heap_bytes) {
        JS_SetGCParameter(rt->cx, JSGC_MAX_BYTES, max_heap_bytes);
    }
    if (native_stack_quota_bytes) {
        JS_SetNativeStackQuota(rt->cx, native_stack_quota_bytes);
#ifdef JS_HAS_MUTABLE_WASI_RECURSION_LIMIT
        /* On wasi, SpiderMonkey bounds recursion with a depth counter, not
         * the native-stack quota — JS_SetNativeStackQuota alone is a no-op
         * for recursion depth. Engines built by scripts/build-engine-intl.sh
         * carry a patch that makes the counter's ceiling per-context; scale
         * it with the quota at upstream's own tuning ratio (350 units per
         * 1 MiB of stack, the shell's link size), clamped between upstream's
         * default and what the 8 MiB stack this wasm links with can carry. */
        uint64_t depth = (uint64_t)native_stack_quota_bytes * 350 / (1u << 20);
        if (depth < 350) {
            depth = 350;
        }
        if (depth > 2800) {
            depth = 2800;
        }
        JS::RootingContext::get(rt->cx)->wasiRecursionDepthLimit = (uint32_t)depth;
#endif
    }

    /* Promise jobs are queued and drained by us at the end of each js_eval; no
     * timers, no event loop, so an unsettled promise cannot hang the call. */
    /* A shell-like embedding, not a browser main thread: the main agent may
     * block in Atomics.wait (test262 CanBlockIsTrue). */
    JS_SetFutexCanWait(rt->cx);
#ifdef SPIDERMONKEY_WASM_THREADS
    rt->jsrt = JS_GetRuntime(rt->cx);
    /* Every internal dispatch (e.g. a notify resolving another runtime's
     * waitAsync) wakes all parked agent pumps; each re-checks and re-parks.
     * Without this an agent parked on the event futex never hears work
     * arriving on its runtime's internal dispatch queue. The callback carries
     * no runtime context, which is why the wake futex is process-wide. */
    static bool wakeup_registered = false;
    if (!wakeup_registered) {
        js::SetWasm2GoDispatchWakeup(agent_event_notify_all);
        wakeup_registered = true;
    }
#endif
    if (!js::UseInternalJobQueues(rt->cx)) {
        JS_DestroyContext(rt->cx);
        delete rt;
        return 0;
    }

    if (!JS::InitSelfHostedCode(rt->cx)) {
        JS_DestroyContext(rt->cx);
        delete rt;
        return 0;
    }

    JS::SetWarningReporter(rt->cx, warning_reporter);

    /* Module loading is registry-backed (see js.h): the hook serves both
     * static imports and dynamic import() from what the host registered. */
    JS::SetModuleLoadHook(JS_GetRuntime(rt->cx), load_imported_module);
    JS::SetModuleMetadataHook(JS_GetRuntime(rt->cx), module_metadata);

    if (!JS_AddInterruptCallback(rt->cx, interrupt_cb)) {
        JS_DestroyContext(rt->cx);
        delete rt;
        return 0;
    }

    static JSClass global_class = {"global", JSCLASS_GLOBAL_FLAGS, &JS::DefaultGlobalClassOps};

    JS::RealmOptions options;
    /* Expose SharedArrayBuffer + Atomics when the engine carries them (the
     * with-intl source build does; StarlingMonkey's prebuilt is configured
     * --disable-shared-memory and ignores this). A single agent needs no
     * threads for them: non-blocking Atomics are ordinary operations, and a
     * blocking wait either throws or times out per [[CanBlock]]. */
    options.creationOptions().setSharedMemoryAndAtomicsEnabled(true);
    JS::RootedObject global(
        rt->cx, JS_NewGlobalObject(rt->cx, &global_class, nullptr, JS::FireOnNewGlobalHook, options));
    if (!global) {
        JS_DestroyContext(rt->cx);
        delete rt;
        return 0;
    }

    {
        JSAutoRealm ar(rt->cx, global);
        if (!JS::InitRealmStandardClasses(rt->cx)) {
            JS_DestroyContext(rt->cx);
            delete rt;
            return 0;
        }
        /* Inside the realm: handling an interrupt dereferences cx->realm(). */
        discover_interrupt_bits(rt->cx);
        if (rt->bits_addr == nullptr) {
            /* Fallback: stay armed so the callback keeps being invoked. The
             * callback re-arms on every resume. */
            JS_RequestInterruptCallback(rt->cx);
        }
    }

    rt->global = new JS::PersistentRootedObject(rt->cx, global);
    rt->interrupt = 0;
    /* The handle IS the runtime. */
    return reinterpret_cast<uint64_t>(rt);
}

std::string js_run_jobs(uint64_t h) {
    Runtime *rt = rt_from(h);
    if (!rt || !rt->cx) {
        return "{\"ok\":false,\"result\":\"\",\"error\":\"no runtime\"}";
    }

    JS::RootedObject global(rt->cx, rt->global->get());
    JSAutoRealm ar(rt->cx, global);

    /* RunJobs drains microtasks AND the engine's internal dispatch queue —
     * a cross-thread waitAsync resolution lands there. It blocks while an
     * off-thread task is outstanding, which is the wait the host wants: the
     * notifying agent, or the engine's own timeout, releases it. */
    js::RunJobs(rt->cx);
    if (JS_IsExceptionPending(rt->cx)) {
        return make_result(false, "", take_error(rt->cx));
    }
    /* Two-way result: "2" = engine work is still PENDING (an engine-delayed
     * dispatchable — an Atomics.waitAsync timeout — is queued), so wait and
     * call again; "0" = the engine is idle. js::RunJobs above already drained
     * every ready microtask to exhaustion, so an idle report is final for the
     * engine. Host timers (setTimeout) are the HOST's to fire between calls;
     * the engine knows nothing about them, and the host tracks its own. */
    bool pending = false;
#ifdef SPIDERMONKEY_WASM_THREADS
    pending = js::Wasm2GoEarliestDelayedDispatchMs(rt->cx) >= 0;
#endif
    return make_result(true, pending ? "2" : "0", "");
}

std::string js_eval(uint64_t h, const char *src, uint32_t src_len) {
    Runtime *rt = rt_from(h);
    if (!rt || !rt->cx) {
        return "{\"ok\":false,\"result\":\"\",\"error\":\"no runtime\"}";
    }


    JS::RootedObject global(rt->cx, rt->global->get());
    JSAutoRealm ar(rt->cx, global);

    /* Fallback mode: the host cannot trip interruptBits_ itself, so the engine
     * only polls while the interrupt is armed — and an interrupt that fires
     * consumes the arming (handleInterrupt clears the bits, and the terminating
     * path does not re-arm). Arm here so every eval is interruptible, not just
     * the first. In discovery mode the host arms it by storing the bit, and
     * arming here would only add a pointless trip through handleInterrupt. */
    if (rt->bits_addr == nullptr) {
        JS_RequestInterruptCallback(rt->cx);
    }

    JS::CompileOptions opts(rt->cx);
    opts.setFileAndLine("<eval>", 1);

    /* Length-aware on purpose: JS source may legally contain NUL bytes (inside
     * string/template literals), so the byte count comes from the std::string,
     * never from strlen: the byte count is the caller's explicit src_len. */
    JS::SourceText<mozilla::Utf8Unit> buf;
    if (!buf.init(rt->cx, src ? src : "", src ? src_len : 0, JS::SourceOwnership::Borrowed)) {
        JS_ClearPendingException(rt->cx);
        return make_result(false, "", "could not read source");
    }

    JS::RootedValue rval(rt->cx);
    bool ok = JS::Evaluate(rt->cx, opts, buf, &rval);
    if (ok) {
        /* Run whatever microtasks the script queued. A job that throws leaves an
         * exception pending, which we surface exactly like a top-level throw. */
        js::RunJobs(rt->cx);
        ok = !JS_IsExceptionPending(rt->cx);
    }
    if (!ok) {
        return make_result(false, "", take_error(rt->cx));
    }
    /* The completion value crosses as a value ENCODING, not its ToString: a
     * primitive keeps its data and type, an object/function keeps its identity
     * (a handle the host can navigate and call). */
    std::string encoded;
    encode_js_value(rt->cx, rval, encoded);
    return make_result(true, encoded, "");
}

void js_close(uint64_t h) {
    Runtime *rt = rt_from(h);
    if (!rt || !rt->cx) {
        return;
    }
#ifdef SPIDERMONKEY_WASM_THREADS
    /* Deterministic agent shutdown, then JOIN: an agent outliving this
     * runtime would touch a destroyed agent cluster (SAB waiter list,
     * off-thread promise state). shutdown+notify wakes agents parked on the
     * event futex; the urgent interrupt reaches ones parked inside the
     * engine (Atomics.wait, RunJobs' internal drain). */
    {
        rt->agents.shutdown.store(true, std::memory_order_seq_cst);
        /* RE-SIGNAL until every agent has exited, parking on the event futex
         * (50 ms bound) between rounds. One shot is not enough: the
         * urgent interrupt only WAKES a wait in progress — fired in the window
         * between an agent's last loop-head check and its Atomics.wait
         * entry, it is recorded but wakes nothing, and the wait (infinite,
         * for a hostile guest) would never end. Each round re-fires the
         * idempotent interrupt, so an agent inside a wait is terminated by
         * the next round at the latest; the futex wait returns early the
         * moment any agent exits (they bump-and-notify on the way out). */
        while (rt->agents.alive.load(std::memory_order_seq_cst) != 0) {
            {
                std::lock_guard<std::mutex> lock(rt->agents.mu);
                for (JSContext *acx : rt->agents.contexts) {
                    /* URGENT, not CanWait: only CallbackUrgent takes the
                     * futex lock and wakes a wait in progress
                     * (JSContext::requestInterrupt); CallbackCanWait merely
                     * sets a bit for the next poll, which a parked agent
                     * never reaches. */
                    JS_RequestInterruptCallback(acx);
                }
            }
            agent_event_notify_all();
            /* seen AFTER the bump above, or the wait below would return
             * immediately every round; an agent exiting in between bumps
             * again, so the compare-and-park still cannot miss it. */
            uint32_t seen = g_agent_epoch.load(std::memory_order_seq_cst);
            if (rt->agents.alive.load(std::memory_order_seq_cst) == 0) {
                break;
            }
            agent_event_wait(seen, 50 * 1000 * 1000);
        }
        std::vector<pthread_t> threads;
        {
            std::lock_guard<std::mutex> lock(rt->agents.mu);
            threads = rt->agents.threads;
        }
        for (pthread_t t : threads) {
            pthread_join(t, nullptr);
        }
        {
            std::lock_guard<std::mutex> lock(rt->agents.mu);
            rt->agents.threads.clear();
            rt->agents.shutdown.store(false, std::memory_order_seq_cst);
            /* Communication state (queues, latches, clone handles) is the
             * HOST's; it dies with the instance, not here. */
        }
    }
#endif
    /* Persistent roots must be released while their runtime is still alive. */
    if (rt->modules) {
        for (auto &entry : *rt->modules) {
            delete entry.second.js;
            delete entry.second.json;
        }
        delete rt->modules;
        rt->modules = nullptr;
    }
    delete rt->global;
    rt->global = nullptr;
    JS_DestroyContext(rt->cx);
    rt->cx = nullptr;
    delete rt;
    /* JS_ShutDown is intentionally NOT called here: it is process teardown, and
     * the instance may create a fresh runtime afterwards. */
}

uint32_t js_interrupt_addr(uint64_t h) {
    Runtime *rt = rt_from(h);
    if (!rt) {
        return 0;
    }
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&rt->interrupt));
}

uint32_t js_interrupt_bits_addr(uint64_t h) {
    Runtime *rt = rt_from(h);
    if (!rt) {
        return 0;
    }
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(rt->bits_addr));
}

uint32_t js_interrupt_bits_value(uint64_t h) {
    Runtime *rt = rt_from(h);
    if (!rt) {
        return 0;
    }
    return rt->bits_value;
}
