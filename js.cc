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

#include <js/ArrayBuffer.h>
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
#include <js/Initialization.h>
#include <js/Interrupt.h>
#include <js/Modules.h>
#include <js/Promise.h>
#include <js/PropertyAndElement.h>
#include <js/Realm.h>
#include <js/RootingAPI.h>
#include <js/ScriptPrivate.h>
#include <js/SourceText.h>
#include <js/Stack.h>
#include <js/Warnings.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

/* ---- runtime state ------------------------------------------------------- */

static JSContext *g_cx = nullptr;
/* Heap-allocated so its destructor never runs at process exit after the context
 * is gone (a PersistentRooted must not outlive its runtime). */
static JS::PersistentRootedObject *g_global = nullptr;

/* Output captured from the print()/console.* builtins for the current js_eval.
 * SpiderMonkey has no I/O of its own, so these are the only way a script can
 * emit anything. */
static std::string g_stdout;
static std::string g_stderr;

/* Set by the host (a plain 32-bit store into linear memory at
 * js_interrupt_addr) to mean "the host asked for this interrupt". `volatile` so
 * the interrupt callback always reloads it. */
static volatile uint32_t g_interrupt = 0;

/* &JSContext::interruptBits_ and the bit JS_RequestInterruptCallback sets in it,
 * as established by discover_interrupt_bits(). Zero means discovery failed and
 * the fallback (keep the interrupt permanently armed) is in force. */
static uint32_t *g_bits_addr = nullptr;
static uint32_t g_bits_value = 0;

/* True only while discover_interrupt_bits() is probing, so the callback neither
 * terminates the (nonexistent) script nor re-arms and perturbs the very word
 * being measured. */
static bool g_discovering = false;

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
 * g_interrupt, not the mere fact of being called, is what authorises
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
 *   - The host, momentarily. Fire() stores g_interrupt before the interruptBits_
 *     bit, but the guest runs on another thread and reads the two words with
 *     independent, unordered loads; it can observe the bit while g_interrupt is
 *     still stale. Re-arming and resuming turns that into a one-loop-head delay
 *     rather than a lost interrupt — and a lost interrupt means Eval never
 *     returns while the host believes it cancelled the script.
 *
 * Re-arming can therefore only spin for as long as the flag takes to become
 * visible. It cannot spin forever unless something trips interruptBits_ and
 * never sets g_interrupt, which nothing in this design does. */
#ifdef SPIDERMONKEY_WASM_THREADS
static bool agents_shutting_down(); /* defined with AgentState below */
#endif

static bool interrupt_cb(JSContext *cx) {
    if (g_discovering) {
        return true;
    }
#ifdef SPIDERMONKEY_WASM_THREADS
    /* js_close interrupts every agent context (CanWait) so an agent parked
     * inside the engine — an Atomics.wait with time left — unblocks NOW.
     * Without this arm the callback would judge the interrupt "not ours",
     * re-arm, and RESUME the wait: close would stall for the remaining
     * timeout. Terminate the agent script instead (uncatchable, like the
     * host interrupt). */
    if (agents_shutting_down()) {
        return false;
    }
#endif
    if (g_interrupt) {
        g_interrupt = 0;
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
 * On success g_bits_addr/g_bits_value are set and the host can trip the poll
 * with a single store, leaving the steady-state cost at the one relaxed load the
 * interpreter already does per loop head. On failure they stay 0 and js_new arms
 * the interrupt permanently instead — correct, just slower. */
static void discover_interrupt_bits(JSContext *cx) {
    /* Test hook: forcing discovery to fail is the only way to exercise the
     * always-armed fallback, which would otherwise be code that never runs
     * until the day a SpiderMonkey upgrade moves interruptBits_. */
    if (std::getenv("SPIDERMONKEY_WASM_NO_INTERRUPT_DISCOVERY")) {
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

    g_discovering = true;

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

    g_discovering = false;

    /* Ambiguity is indistinguishable from a wrong guess, so refuse both: a
     * mislocated word would make the host scribble into unrelated engine state.
     * Exactly one survivor, and it must be back at rest. */
    if (matches != 1 || *found != 0) {
        return;
    }
    g_bits_addr = found;
    g_bits_value = found_value;
}

/* ---- captured output builtins -------------------------------------------- */

/* Join the call's arguments with spaces, as console.log does, and append a
 * newline. A value that cannot be stringified (a throwing toString) aborts the
 * call with that exception. */
static bool collect_args(JSContext *cx, const JS::CallArgs &args, std::string *out) {
    for (unsigned i = 0; i < args.length(); i++) {
        JS::RootedString str(cx, JS::ToString(cx, args[i]));
        if (!str) {
            return false;
        }
        JS::UniqueChars utf8 = JS_EncodeStringToUTF8(cx, str);
        if (!utf8) {
            return false;
        }
        if (i > 0) {
            *out += ' ';
        }
        *out += utf8.get();
    }
    *out += '\n';
    return true;
}

static bool builtin_print(JSContext *cx, unsigned argc, JS::Value *vp) {
    JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
    if (!collect_args(cx, args, &g_stdout)) {
        return false;
    }
    args.rval().setUndefined();
    return true;
}

static bool builtin_print_err(JSContext *cx, unsigned argc, JS::Value *vp) {
    JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
    if (!collect_args(cx, args, &g_stderr)) {
        return false;
    }
    args.rval().setUndefined();
    return true;
}

/* `print` plus a console object with the four methods scripts reach for. This is
 * the ENTIRE host surface a guest script gets: no fetch, no timers, no file or
 * network access, nothing that can escape the wasm sandbox. Anything more must
 * be added deliberately, and must then be gated by the host-side policy hooks. */

#ifdef SPIDERMONKEY_WASM_THREADS
/* ---- helper-thread pool ----------------------------------------------------
 * A threads build hands internal tasks to an EXTERNAL pool and waits on them.
 * Each task runs on its own pthread — a goroutine under wasm2go. Registered
 * BEFORE JS_Init: the engine decides useInternalThreadPool_ from whether a
 * callback is already set.
 */
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

/* ---- host timers -----------------------------------------------------------
 *
 * PROMISE work stays entirely inside the engine: js::UseInternalJobQueues
 * installs both the internal job queue and the internal dispatch queue, and
 * js::RunJobs drains both — cross-thread Atomics.waitAsync resolutions and
 * their timeouts included. Do NOT also register an external
 * DispatchToEventLoop callback: InternalJobQueue::runJobs calls
 * OffThreadPromiseRuntimeState::internalDrain unconditionally, which then
 * blocks on a condition variable that only the INTERNAL queue ever signals.
 *
 * What the engine does not supply is setTimeout, which test262's
 * atomicsHelper.js needs — without one it installs a promise-chain busy-wait
 * polyfill that spins inside RunJobs. So the host keeps exactly one queue: JS
 * timers, fired from the pump loops.
 */
struct HostTimerQueue {
    std::mutex mu;
    struct Timer {
        uint64_t due_ms;
        JS::PersistentRootedValue *fn;
    };
    std::vector<Timer> timers;
};
static HostTimerQueue g_timers;

/* The timer queue owning the CURRENT thread (agents carry their own). */
static thread_local HostTimerQueue *t_timers = &g_timers;

static uint64_t monotonic_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static bool builtin_set_timeout(JSContext *cx, unsigned argc, JS::Value *vp) {
    JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
    if (!args.get(0).isObject() || !JS::IsCallable(&args[0].toObject())) {
        JS_ReportErrorASCII(cx, "setTimeout: expected a function");
        return false;
    }
    double ms = 0;
    if (args.hasDefined(1) && !JS::ToNumber(cx, args.get(1), &ms)) {
        return false;
    }
    if (ms < 0) {
        ms = 0;
    }
    auto *fn = new JS::PersistentRootedValue(cx, args[0]);
    {
        std::lock_guard<std::mutex> lock(t_timers->mu);
        t_timers->timers.push_back({monotonic_ms() + (uint64_t)ms, fn});
    }
    args.rval().setUndefined();
    return true;
}

/* Fire every due timer (or drop them all on shutdown). Reports whether any
 * callback ran. */
static bool host_timers_run(JSContext *cx, HostTimerQueue *q, bool shutting_down) {
    /* ONE BATCH PER CALL: only timers already due when this call started run
     * now; a timer armed by one of these callbacks waits for the NEXT call,
     * however soon it is due. Draining to exhaustion instead would let a
     * zero-delay self-rescheduling timer (test262's `$262.agent.setTimeout(f,
     * 0)` polling idiom) monopolize the pump: the caller alternates timers
     * with js::RunJobs, and it is that interleave which lets engine-delayed
     * work (Atomics.waitAsync timeouts travel the internal dispatch queue)
     * resolve while such a timer loop is polling for exactly that result. */
    std::vector<JS::PersistentRootedValue *> batch;
    {
        std::lock_guard<std::mutex> lock(q->mu);
        uint64_t now = monotonic_ms();
        for (size_t i = 0; i < q->timers.size(); i++) {
            if (shutting_down) {
                delete q->timers[i].fn;
                q->timers.erase(q->timers.begin() + i);
                i--;
            } else if (q->timers[i].due_ms <= now) {
                batch.push_back(q->timers[i].fn);
                q->timers.erase(q->timers.begin() + i);
                i--;
            }
        }
    }
    bool ran = false;
    for (JS::PersistentRootedValue *fn : batch) {
        JS::RootedValue f(cx, fn->get());
        delete fn;
        JS::RootedValue rval(cx);
        if (!JS_CallFunctionValue(cx, nullptr, f, JS::HandleValueArray::empty(), &rval)) {
            JS_ClearPendingException(cx);
        }
        ran = true;
    }
    return ran;
}

static bool host_timers_pending(HostTimerQueue *q) {
    std::lock_guard<std::mutex> lock(q->mu);
    return !q->timers.empty();
}

static bool install_builtins(JSContext *cx, JS::HandleObject global) {
    if (!JS_DefineFunction(cx, global, "print", builtin_print, 0, 0)) {
        return false;
    }
    JS::RootedObject console(cx, JS_NewPlainObject(cx));
    if (!console) {
        return false;
    }
    if (!JS_DefineFunction(cx, console, "log", builtin_print, 0, 0) ||
        !JS_DefineFunction(cx, console, "info", builtin_print, 0, 0) ||
        !JS_DefineFunction(cx, console, "warn", builtin_print_err, 0, 0) ||
        !JS_DefineFunction(cx, console, "error", builtin_print_err, 0, 0)) {
        return false;
    }
    JS::RootedValue consoleVal(cx, JS::ObjectValue(*console));
    return JS_DefineProperty(cx, global, "console", consoleVal, JSPROP_ENUMERATE);
}

/* Warnings (including the "terminated" warning HandleInterrupt emits when our
 * callback returns false) go to the captured stderr, never to the host process. */
static void warning_reporter(JSContext *cx, JSErrorReport *report) {
    (void)cx;
    if (report && report->message().c_str()) {
        g_stderr += report->message().c_str();
        g_stderr += '\n';
    }
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

/* Module specifiers that missed the registry since the last result was built.
 * A DYNAMIC import failure is caught by guest code (the promise rejection is
 * the test's to inspect), so the host cannot learn the missing specifier from
 * the error text; this side channel carries it out so a host loader can
 * fetch + register + retry. */
static std::vector<std::string> g_missing_modules;

static std::string make_result(bool ok, const std::string &result, const std::string &error) {
    std::string j = "{\"ok\":";
    j += ok ? "true" : "false";
    j += ",";
    j += json_field("result", result, true);
    j += json_field("stdout", g_stdout, true);
    j += json_field("stderr", g_stderr, true);
    j += json_field("error", error, false);
    if (!g_missing_modules.empty()) {
        j += ",\"missing_modules\":[";
        for (size_t i = 0; i < g_missing_modules.size(); i++) {
            if (i) {
                j += ",";
            }
            j += "\"";
            json_escape(g_missing_modules[i], j);
            j += "\"";
        }
        j += "]";
        g_missing_modules.clear();
    }
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
struct ModuleEntry {
    std::string source;
    JS::PersistentRootedObject *js = nullptr;   /* ModuleType::JavaScript */
    JS::PersistentRootedObject *json = nullptr; /* ModuleType::JSON */
};
static std::map<std::string, ModuleEntry> *g_modules = nullptr;

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
static void register_module_source(const std::string &specifier, const std::string &src) {
    if (!g_modules) {
        g_modules = new std::map<std::string, ModuleEntry>();
    }
    ModuleEntry &e = (*g_modules)[specifier];
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
    register_module_source(specifier, src);
    return compile_module_entry(cx, specifier, (*g_modules)[specifier],
                                JS::ModuleType::JavaScript);
}

/* Look up (and lazily compile) the module a request resolves to, or report
 * (and leave pending) a "module not registered" error. */
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
    if (g_modules) {
        auto it = g_modules->find(resolved);
        if (it != g_modules->end()) {
            return compile_module_entry(cx, resolved, it->second,
                                        JS::GetModuleRequestType(cx, moduleRequest));
        }
    }
    g_missing_modules.push_back(resolved);
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

std::string js_module_register(uint64_t h, const char *specifier_p, uint32_t specifier_len,
                               const char *src_p, uint32_t src_len) {
    const std::string specifier(specifier_p ? specifier_p : "", specifier_p ? specifier_len : 0);
    const std::string src(src_p ? src_p : "", src_p ? src_len : 0);
    if (!g_cx || h == 0) {
        g_stdout.clear();
        g_stderr.clear();
        return make_result(false, "", "no runtime");
    }
    g_stdout.clear();
    g_stderr.clear();
    /* Source only; compilation is deferred to the first import so a compile
     * error surfaces there with its real type (SyntaxError for script-only
     * source, per HostLoadImportedModule), and so the same source can serve
     * as JS or JSON depending on the request's import attributes. */
    register_module_source(specifier, src);
    return make_result(true, "registered", "");
}

std::string js_eval_module(uint64_t h, const char *specifier_p, uint32_t specifier_len,
                           const char *src_p, uint32_t src_len) {
    const std::string specifier(specifier_p ? specifier_p : "", specifier_p ? specifier_len : 0);
    const std::string src(src_p ? src_p : "", src_p ? src_len : 0);
    if (!g_cx || h == 0) {
        g_stdout.clear();
        g_stderr.clear();
        return make_result(false, "", "no runtime");
    }
    g_stdout.clear();
    g_stderr.clear();
    JS::RootedObject global(g_cx, g_global->get());
    JSAutoRealm ar(g_cx, global);
    if (g_bits_addr == nullptr) {
        JS_RequestInterruptCallback(g_cx);
    }

    JS::RootedObject module(g_cx, compile_and_register_module(g_cx, specifier, src));
    if (!module) {
        return make_result(false, "", take_error(g_cx));
    }
    JS::RootedValue hd(g_cx, JS::ObjectValue(*module));
    if (!JS::LoadRequestedModules(g_cx, module, hd, load_module_resolved,
                                  load_module_rejected) ||
        JS_IsExceptionPending(g_cx)) {
        return make_result(false, "", take_error(g_cx));
    }
    if (!JS::ModuleLink(g_cx, module)) {
        return make_result(false, "", take_error(g_cx));
    }
    JS::RootedValue rval(g_cx);
    if (!JS::ModuleEvaluate(g_cx, module, &rval)) {
        return make_result(false, "", take_error(g_cx));
    }
    js::RunJobs(g_cx);
    if (JS_IsExceptionPending(g_cx)) {
        return make_result(false, "", take_error(g_cx));
    }

    /* With top-level await the result is the evaluation promise; report by its
     * settled state. No timers exist, so a still-pending promise can never
     * settle: that is an error, not something to wait on. */
    if (rval.isObject()) {
        JS::RootedObject promise(g_cx, &rval.toObject());
        if (JS::IsPromiseObject(promise)) {
            switch (JS::GetPromiseState(promise)) {
            case JS::PromiseState::Fulfilled:
                return make_result(true, "undefined", "");
            case JS::PromiseState::Rejected: {
                JS::RootedValue reason(g_cx, JS::GetPromiseResult(promise));
                JS_SetPendingException(g_cx, reason);
                return make_result(false, "", take_error(g_cx));
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

/* ---- $262 test hooks ------------------------------------------------------ */

static bool test262_gc(JSContext *cx, unsigned argc, JS::Value *vp) {
    JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
    JS_GC(cx);
    args.rval().setUndefined();
    return true;
}

static bool test262_detach_array_buffer(JSContext *cx, unsigned argc, JS::Value *vp) {
    JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
    if (!args.get(0).isObject()) {
        JS_ReportErrorASCII(cx, "detachArrayBuffer: argument must be an ArrayBuffer");
        return false;
    }
    JS::RootedObject obj(cx, &args[0].toObject());
    if (!JS::DetachArrayBuffer(cx, obj)) {
        return false;
    }
    args.rval().setUndefined();
    return true;
}

static bool test262_eval_script(JSContext *cx, unsigned argc, JS::Value *vp) {
    JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
    JS::RootedString str(cx, JS::ToString(cx, args.get(0)));
    if (!str) {
        return false;
    }
    std::string src = jsstring_to_utf8(cx, str);
    JS::CompileOptions opts(cx);
    opts.setFileAndLine("<evalScript>", 1);
    JS::SourceText<mozilla::Utf8Unit> buf;
    if (!buf.init(cx, src.data(), src.size(), JS::SourceOwnership::Borrowed)) {
        return false;
    }
    /* Evaluates in the realm this $262's function was created in: entering the
     * call entered that realm, which is exactly test262's cross-realm usage
     * ($262.createRealm().evalScript(...) runs in the child realm). */
    return JS::Evaluate(cx, opts, buf, args.rval());
}

/* [[IsHTMLDDA]]: an object that emulates undefined and yields null when
 * called, per test262's host-defined `IsHTMLDDA` requirement. */
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

static JSObject *install_test262(JSContext *cx, JS::HandleObject global);

static bool test262_create_realm(JSContext *cx, unsigned argc, JS::Value *vp) {
    JS::CallArgs args = JS::CallArgsFromVp(argc, vp);

    static JSClass global_class = {"global", JSCLASS_GLOBAL_FLAGS, &JS::DefaultGlobalClassOps};
    JS::RealmOptions options;
    /* Same-compartment realm: objects flow between parent and child directly,
     * no cross-compartment wrappers, which is what test262 harness code
     * expects of $262.createRealm(). */
    JS::RootedObject current(cx, JS::CurrentGlobalOrNull(cx));
    options.creationOptions().setExistingCompartment(current);
    /* Child realms get the same SharedArrayBuffer/Atomics surface as the
     * primary realm (js_new); without this, cross-realm SAB tests see the
     * constructor missing on the child global. */
    options.creationOptions().setSharedMemoryAndAtomicsEnabled(true);
    JS::RootedObject newGlobal(
        cx, JS_NewGlobalObject(cx, &global_class, nullptr, JS::FireOnNewGlobalHook, options));
    if (!newGlobal) {
        return false;
    }
    JS::RootedObject childHooks(cx);
    {
        JSAutoRealm ar(cx, newGlobal);
        if (!JS::InitRealmStandardClasses(cx) || !install_builtins(cx, newGlobal)) {
            return false;
        }
        childHooks = install_test262(cx, newGlobal);
        if (!childHooks) {
            return false;
        }
    }
    args.rval().setObject(*childHooks);
    return true;
}

/* Build the $262 object for `global` and define it there. Returns it. */

/* ---- $262.agent (test262 agents; threads builds only) ---------------------
 *
 * test262's agent model: $262.agent.start(src) runs src on a NEW agent — its
 * own thread, its own JSRuntime/JSContext/global — sharing nothing with the
 * parent but SharedArrayBuffer memory. Agents talk through
 * $262.agent.broadcast(sab) / receiveBroadcast(cb) and report(str) /
 * getReport().
 *
 * A thread here is a pthread, which wasi-libc turns into a wasi_thread_spawn —
 * which wasm2go runs on a GOROUTINE. So the whole chain is: guest JS agent ->
 * SpiderMonkey thread -> pthread -> wasi_thread_spawn -> goroutine.
 *
 * Only compiled when the engine was built for wasi-threads
 * (scripts/build-engine-intl.sh with SPIDERMONKEY_THREADS=1); the
 * single-agent build leaves $262.agent absent, and the test262 runner skips
 * the agent tests exactly as before.
 */
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

/* The broadcast rendezvous: the parent publishes one SAB (+ an int32 payload,
 * which is all test262 sends) and every started agent picks it up in its own
 * receiveBroadcast callback. Reports flow back the other way. */
struct AgentState {
    std::mutex mu;
    std::deque<std::string> reports;

    /* Deterministic agent lifecycle — no polling anywhere:
     *  - epoch is a futex word. Agents park on it (memory.atomic.wait32,
     *    which the Go side implements with channels); broadcast, leaving and
     *    shutdown bump-and-notify it. No sleep loops.
     *  - threads/contexts track every live agent so js_close can wake each
     *    one (the URGENT interrupt reaches even an agent parked
     *    inside the engine) and JOIN it before the runtime dies. An agent
     *    outliving its interpreter used to touch a destroyed runtime. */
    std::atomic<uint32_t> epoch{0};
    std::atomic<bool> shutdown{false};
    std::atomic<uint32_t> alive{0}; /* running agent threads; futex-signaled on exit */
    std::vector<pthread_t> threads;
    std::vector<JSContext *> contexts;

    /* The broadcast SAB, serialized. Public JSAPI has no "wrap this memory as a
     * SharedArrayBuffer" entry point; the SPEC route — and the one the
     * SpiderMonkey shell takes — is a structured clone with shared-memory
     * objects allowed, which hands every agent a SAB backed by the SAME
     * memory (that is what makes it *shared*, as opposed to copied). */
    std::unique_ptr<JSAutoStructuredCloneBuffer> sab_clone;
    int32_t sab_payload = 0;
    bool broadcast_ready = false;
};

static AgentState g_agents;

static bool agents_shutting_down() {
    return g_agents.shutdown.load(std::memory_order_seq_cst);
}

/* Bump the event epoch and wake every agent parked on it. Called on
 * broadcast, on leaving, and on shutdown. */
static void agent_event_notify_all() {
    g_agents.epoch.fetch_add(1, std::memory_order_seq_cst);
    __builtin_wasm_memory_atomic_notify((int *)&g_agents.epoch, INT32_MAX);
}

/* Park until the epoch moves past `seen` (or the timeout, in ns, expires;
 * negative = forever). The futex compare-and-park makes this race-free:
 * an epoch bump between load and wait returns immediately. */
static void agent_event_wait(uint32_t seen, int64_t timeout_ns) {
    __builtin_wasm_memory_atomic_wait32((int *)&g_agents.epoch, (int32_t)seen, timeout_ns);
}

/* Agents parent their runtimes to the main one — same agent cluster, which is
 * what makes the SharedArrayBuffer waiter list and the off-thread promise
 * bookkeeping (both cluster-scoped) reach them. */
static JSRuntime *g_parent_runtime = nullptr;

/* Agent diagnostics go to STDERR, never to the $262.agent report queue:
 * test262 compares reports BY VALUE, so a diagnostic in the queue silently
 * corrupts the assertion under test. */
static void agent_milestone(const char *what) {
    fprintf(stderr, "[agent] %s\n", what);
}


struct AgentStart {
    std::string src;
};

static bool agent_report(JSContext *cx, unsigned argc, JS::Value *vp) {
    JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
    JS::RootedString str(cx, JS::ToString(cx, args.get(0)));
    if (!str) {
        return false;
    }
    JS::UniqueChars utf8 = JS_EncodeStringToUTF8(cx, str);
    if (!utf8) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(g_agents.mu);
        g_agents.reports.emplace_back(utf8.get());
    }
    args.rval().setUndefined();
    return true;
}

static bool agent_get_report(JSContext *cx, unsigned argc, JS::Value *vp) {
    JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
    std::string msg;
    {
        std::lock_guard<std::mutex> lock(g_agents.mu);
        if (g_agents.reports.empty()) {
            args.rval().setNull();
            return true;
        }
        msg = g_agents.reports.front();
        g_agents.reports.pop_front();
    }
    JS::RootedString str(cx, JS_NewStringCopyN(cx, msg.data(), msg.size()));
    if (!str) {
        return false;
    }
    args.rval().setString(str);
    return true;
}

static bool agent_sleep(JSContext *cx, unsigned argc, JS::Value *vp) {
    JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
    double ms = 0;
    if (!JS::ToNumber(cx, args.get(0), &ms)) {
        return false;
    }
    if (ms > 0) {
        struct timespec ts;
        ts.tv_sec = (time_t)(ms / 1000);
        ts.tv_nsec = (long)((ms - (double)ts.tv_sec * 1000) * 1e6);
        nanosleep(&ts, nullptr);
    }
    args.rval().setUndefined();
    return true;
}

static thread_local bool t_agent_left = false;

static bool agent_leaving(JSContext *cx, unsigned argc, JS::Value *vp) {
    JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
    t_agent_left = true;
    agent_event_notify_all();
    args.rval().setUndefined();
    return true;
}

static bool agent_monotonic_now(JSContext *cx, unsigned argc, JS::Value *vp) {
    JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    args.rval().setNumber((double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6);
    return true;
}

/* Parent side: publish the SAB every agent's receiveBroadcast will wrap. */
static bool agent_broadcast(JSContext *cx, unsigned argc, JS::Value *vp) {
    JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
    if (!args.get(0).isObject()) {
        JS_ReportErrorASCII(cx, "$262.agent.broadcast: expected a SharedArrayBuffer");
        return false;
    }
    JS::RootedObject sab(cx, &args[0].toObject());
    if (!JS::IsSharedArrayBufferObject(sab)) {
        JS_ReportErrorASCII(cx, "$262.agent.broadcast: expected a SharedArrayBuffer");
        return false;
    }
    int32_t payload = 0;
    if (args.hasDefined(1) && !JS::ToInt32(cx, args.get(1), &payload)) {
        return false;
    }

    /* SameProcess scope + allowSharedMemoryObjects: the clone carries a
     * reference to the shared memory, not a copy of it. */
    auto clone = std::make_unique<JSAutoStructuredCloneBuffer>(
        JS::StructuredCloneScope::SameProcess, nullptr, nullptr);
    JS::CloneDataPolicy policy;
    policy.allowSharedMemoryObjects();
    /* SABs are "intra-cluster clonable shared objects": without this second
     * bit the clone throws the browser-flavoured COOP/COEP TypeError. All
     * agents here live in one process — one agent cluster by construction. */
    policy.allowIntraClusterClonableSharedObjects();
    JS::RootedValue sabVal(cx, JS::ObjectValue(*sab));
    if (!clone->write(cx, sabVal, JS::UndefinedHandleValue, policy)) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(g_agents.mu);
        g_agents.sab_clone = std::move(clone);
        g_agents.sab_payload = payload;
        g_agents.broadcast_ready = true;
    }
    agent_event_notify_all();
    args.rval().setUndefined();
    return true;
}

/* Agent side: install $262.agent with the child-only entry points. */
static bool agent_receive_broadcast(JSContext *cx, unsigned argc, JS::Value *vp);

static JSObject *install_agent_child(JSContext *cx, JS::HandleObject global) {
    JS::RootedObject hooks(cx, JS_NewPlainObject(cx));
    JS::RootedObject agent(cx, JS_NewPlainObject(cx));
    if (!hooks || !agent) {
        return nullptr;
    }
    if (!JS_DefineFunction(cx, agent, "receiveBroadcast", agent_receive_broadcast, 1, 0) ||
        !JS_DefineFunction(cx, agent, "report", agent_report, 1, 0) ||
        !JS_DefineFunction(cx, agent, "sleep", agent_sleep, 1, 0) ||
        !JS_DefineFunction(cx, agent, "leaving", agent_leaving, 0, 0) ||
        !JS_DefineFunction(cx, agent, "monotonicNow", agent_monotonic_now, 0, 0)) {
        return nullptr;
    }
    JS::RootedValue agentVal(cx, JS::ObjectValue(*agent));
    if (!JS_DefineProperty(cx, hooks, "agent", agentVal, JSPROP_ENUMERATE)) {
        return nullptr;
    }
    JS::RootedValue globalVal(cx, JS::ObjectValue(*global));
    if (!JS_DefineProperty(cx, hooks, "global", globalVal, JSPROP_ENUMERATE)) {
        return nullptr;
    }
    JS::RootedValue hooksVal(cx, JS::ObjectValue(*hooks));
    if (!JS_DefineProperty(cx, global, "$262", hooksVal, JSPROP_ENUMERATE)) {
        return nullptr;
    }
    return hooks;
}

/* The agent's receiveBroadcast: block until the parent has broadcast, then
 * hand the callback a SharedArrayBuffer wrapping the SAME memory. */
static bool agent_receive_broadcast(JSContext *cx, unsigned argc, JS::Value *vp) {
    JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
    if (!args.get(0).isObject() || !JS::IsCallable(&args[0].toObject())) {
        JS_ReportErrorASCII(cx, "$262.agent.receiveBroadcast: expected a callback");
        return false;
    }
    JS::RootedValue cb(cx, args[0]);

    /* Park until the parent broadcasts — on the event futex, never a poll.
     * The clone is read under the lock and deserialized into THIS agent's
     * runtime; the resulting SAB shares the parent's memory. */
    JS::RootedValue sabVal(cx);
    int32_t payload = 0;
    for (;;) {
        uint32_t seen = g_agents.epoch.load(std::memory_order_seq_cst);
        {
            std::lock_guard<std::mutex> lock(g_agents.mu);
            if (g_agents.shutdown.load(std::memory_order_seq_cst)) {
                JS_ReportErrorASCII(cx, "receiveBroadcast: interpreter shutting down");
                return false;
            }
            if (g_agents.broadcast_ready && g_agents.sab_clone) {
                JS::CloneDataPolicy policy;
                policy.allowSharedMemoryObjects();
                policy.allowIntraClusterClonableSharedObjects();
                if (!g_agents.sab_clone->read(cx, &sabVal, policy, nullptr, nullptr)) {
                    return false;
                }
                payload = g_agents.sab_payload;
                break;
            }
        }
        agent_event_wait(seen, -1);
    }

    JS::RootedValueArray<2> cbArgs(cx);
    cbArgs[0].set(sabVal);
    cbArgs[1].setInt32(payload);
    JS::RootedValue rval(cx);
    if (!JS_CallFunctionValue(cx, nullptr, cb, cbArgs, &rval)) {
        return false;
    }
    args.rval().setUndefined();
    return true;
}

/* One agent = one thread = one runtime. */
static void *agent_thread_main(void *arg) {
    std::unique_ptr<AgentStart> start(static_cast<AgentStart *>(arg));

    /* The agent's own setTimeout queue (harness code runs on its global). */
    HostTimerQueue timers;
    t_timers = &timers;
    t_agent_left = false;

    JSContext *cx = JS_NewContext(JS::DefaultHeapMaxBytes, g_parent_runtime);
    if (!cx) {
        agent_milestone("JS_NewContext FAILED");
        return nullptr;
    }
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
        std::lock_guard<std::mutex> lock(g_agents.mu);
        g_agents.contexts.push_back(cx);
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
        if (!JS::InitRealmStandardClasses(cx) || !install_builtins(cx, global) ||
            !install_agent_child(cx, global)) {
            agent_milestone("realm FAILED");
            JS_DestroyContext(cx);
            return nullptr;
        }
        JS::CompileOptions opts(cx);
        opts.setFileAndLine("<agent>", 1);
        JS::SourceText<mozilla::Utf8Unit> buf;
        if (buf.init(cx, start->src.data(), start->src.size(), JS::SourceOwnership::Borrowed)) {
            JS::RootedValue rval(cx);
            if (JS::Evaluate(cx, opts, buf, &rval)) {
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
                while (!t_agent_left && !g_agents.shutdown.load(std::memory_order_seq_cst)) {
                    bool ran = host_timers_run(cx, &timers, /* shutting_down */ false);
                    js::RunJobs(cx);
                    JS_ClearPendingException(cx);
                    if (t_agent_left || g_agents.shutdown.load(std::memory_order_seq_cst)) {
                        break;
                    }
                    if (ran) {
                        continue; /* a timer fired: it may have queued jobs */
                    }
                    uint32_t seen = g_agents.epoch.load(std::memory_order_seq_cst);
                    int64_t timeout_ns = -1;
                    {
                        std::lock_guard<std::mutex> lock(timers.mu);
                        uint64_t now = monotonic_ms();
                        for (auto &tm : timers.timers) {
                            int64_t d = (int64_t)(tm.due_ms > now ? tm.due_ms - now : 0) * 1000000;
                            if (timeout_ns < 0 || d < timeout_ns) {
                                timeout_ns = d;
                            }
                        }
                    }
                    int64_t delayed_ms = js::Wasm2GoEarliestDelayedDispatchMs(cx);
                    if (delayed_ms >= 0) {
                        int64_t d = delayed_ms * 1000000;
                        if (timeout_ns < 0 || d < timeout_ns) {
                            timeout_ns = d;
                        }
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
        } else {
            agent_milestone("source-init FAILED");
        }
        JS_ClearPendingException(cx);
    }
    host_timers_run(cx, &timers, /* shutting_down */ true);
    {
        std::lock_guard<std::mutex> lock(g_agents.mu);
        auto &v = g_agents.contexts;
        v.erase(std::remove(v.begin(), v.end(), cx), v.end());
    }
    JS_DestroyContext(cx);
    t_timers = &g_timers;
    /* Last: js_close's shutdown loop parks on the event futex until alive
     * hits zero; the notify is what releases it. */
    g_agents.alive.fetch_sub(1, std::memory_order_seq_cst);
    agent_event_notify_all();
    return nullptr;
}

static bool agent_start(JSContext *cx, unsigned argc, JS::Value *vp) {
    JS::CallArgs args = JS::CallArgsFromVp(argc, vp);
    JS::RootedString str(cx, JS::ToString(cx, args.get(0)));
    if (!str) {
        return false;
    }
    JS::UniqueChars utf8 = JS_EncodeStringToUTF8(cx, str);
    if (!utf8) {
        return false;
    }
    auto *start = new AgentStart{std::string(utf8.get())};

    pthread_t tid;
    /* Incremented BEFORE create: the agent may run to completion (and
     * decrement) before pthread_create even returns here. */
    g_agents.alive.fetch_add(1, std::memory_order_seq_cst);
    if (pthread_create(&tid, nullptr, agent_thread_main, start) != 0) {
        g_agents.alive.fetch_sub(1, std::memory_order_seq_cst);
        delete start;
        JS_ReportErrorASCII(cx, "$262.agent.start: could not spawn an agent");
        return false;
    }
    /* NOT detached: js_close joins every agent so none can outlive the
     * runtime it shares an agent cluster with. */
    {
        std::lock_guard<std::mutex> lock(g_agents.mu);
        g_agents.threads.push_back(tid);
    }
    args.rval().setUndefined();
    return true;
}

/* Parent side of $262.agent. */
static bool install_agent_parent(JSContext *cx, JS::HandleObject hooks) {
    JS::RootedObject agent(cx, JS_NewPlainObject(cx));
    if (!agent) {
        return false;
    }
    if (!JS_DefineFunction(cx, agent, "start", agent_start, 1, 0) ||
        !JS_DefineFunction(cx, agent, "broadcast", agent_broadcast, 2, 0) ||
        !JS_DefineFunction(cx, agent, "getReport", agent_get_report, 0, 0) ||
        !JS_DefineFunction(cx, agent, "sleep", agent_sleep, 1, 0) ||
        !JS_DefineFunction(cx, agent, "monotonicNow", agent_monotonic_now, 0, 0)) {
        return false;
    }
    JS::RootedValue agentVal(cx, JS::ObjectValue(*agent));
    return JS_DefineProperty(cx, hooks, "agent", agentVal, JSPROP_ENUMERATE);
}

#endif /* SPIDERMONKEY_WASM_THREADS */

static JSObject *install_test262(JSContext *cx, JS::HandleObject global) {
    JS::RootedObject hooks(cx, JS_NewPlainObject(cx));
    if (!hooks) {
        return nullptr;
    }
    if (!JS_DefineFunction(cx, hooks, "createRealm", test262_create_realm, 0, 0) ||
        !JS_DefineFunction(cx, hooks, "detachArrayBuffer", test262_detach_array_buffer, 1, 0) ||
        !JS_DefineFunction(cx, hooks, "evalScript", test262_eval_script, 1, 0) ||
        !JS_DefineFunction(cx, hooks, "gc", test262_gc, 0, 0)) {
        return nullptr;
    }
    /* A real setTimeout: the harness needs one (atomicsHelper.js otherwise
     * installs a promise-chain busy-wait that starves the loop). Host
     * capability — hooks only, never the sandbox surface. */
    if (!JS_DefineFunction(cx, global, "setTimeout", builtin_set_timeout, 2, 0)) {
        return nullptr;
    }
    JS::RootedValue globalVal(cx, JS::ObjectValue(*global));
    if (!JS_DefineProperty(cx, hooks, "global", globalVal, JSPROP_ENUMERATE)) {
        return nullptr;
    }
    JS::RootedObject dda(cx, JS_NewObject(cx, &is_htmldda_class));
    if (!dda) {
        return nullptr;
    }
    JS::RootedValue ddaVal(cx, JS::ObjectValue(*dda));
    if (!JS_DefineProperty(cx, hooks, "IsHTMLDDA", ddaVal, JSPROP_ENUMERATE)) {
        return nullptr;
    }
#ifdef SPIDERMONKEY_WASM_THREADS
    /* Threads builds also get $262.agent: real agents on real threads (which
     * become goroutines under wasm2go). */
    if (!install_agent_parent(cx, hooks)) {
        return nullptr;
    }
#endif
    JS::RootedValue hooksVal(cx, JS::ObjectValue(*hooks));
    if (!JS_DefineProperty(cx, global, "$262", hooksVal, JSPROP_ENUMERATE)) {
        return nullptr;
    }
    return hooks;
}

void js_install_test262_hooks(uint64_t h) {
    if (!g_cx || h == 0) {
        return;
    }
    JS::RootedObject global(g_cx, g_global->get());
    JSAutoRealm ar(g_cx, global);
    if (!install_test262(g_cx, global)) {
        JS_ClearPendingException(g_cx);
    }
}

/* ---- public API (js.h) --------------------------------------------------- */

uint64_t js_new(uint32_t max_heap_bytes, uint32_t native_stack_quota_bytes) {
    if (g_cx) {
        return 0; /* one runtime per instance */
    }

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

    /* The nursery is carved out of the heap budget, so a tiny max_heap_bytes
     * with SpiderMonkey's default nursery would leave nothing for the tenured
     * heap. Let SpiderMonkey size it; we only clamp the total. */
    g_cx = JS_NewContext(max_heap_bytes ? max_heap_bytes : JS::DefaultHeapMaxBytes);
    if (!g_cx) {
        return 0;
    }

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
        JS_SetGCParameter(g_cx, JSGC_MAX_BYTES, max_heap_bytes);
    }
    if (native_stack_quota_bytes) {
        JS_SetNativeStackQuota(g_cx, native_stack_quota_bytes);
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
        JS::RootingContext::get(g_cx)->wasiRecursionDepthLimit = (uint32_t)depth;
#endif
    }

    /* Promise jobs are queued and drained by us at the end of each js_eval; no
     * timers, no event loop, so an unsettled promise cannot hang the call. */
    /* A shell-like embedding, not a browser main thread: the main agent may
     * block in Atomics.wait (test262 CanBlockIsTrue). */
    JS_SetFutexCanWait(g_cx);
#ifdef SPIDERMONKEY_WASM_THREADS
    g_parent_runtime = JS_GetRuntime(g_cx);
    /* Every internal dispatch (e.g. a notify resolving another runtime's
     * waitAsync) wakes all parked agent pumps; each re-checks and re-parks.
     * Without this an agent parked on the event futex never hears work
     * arriving on its runtime's internal dispatch queue. */
    js::SetWasm2GoDispatchWakeup(agent_event_notify_all);
#endif
    if (!js::UseInternalJobQueues(g_cx)) {
        JS_DestroyContext(g_cx);
        g_cx = nullptr;
        return 0;
    }

    if (!JS::InitSelfHostedCode(g_cx)) {
        JS_DestroyContext(g_cx);
        g_cx = nullptr;
        return 0;
    }

    JS::SetWarningReporter(g_cx, warning_reporter);

    /* Module loading is registry-backed (see js.h): the hook serves both
     * static imports and dynamic import() from what the host registered. */
    JS::SetModuleLoadHook(JS_GetRuntime(g_cx), load_imported_module);
    JS::SetModuleMetadataHook(JS_GetRuntime(g_cx), module_metadata);

    if (!JS_AddInterruptCallback(g_cx, interrupt_cb)) {
        JS_DestroyContext(g_cx);
        g_cx = nullptr;
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
        g_cx, JS_NewGlobalObject(g_cx, &global_class, nullptr, JS::FireOnNewGlobalHook, options));
    if (!global) {
        JS_DestroyContext(g_cx);
        g_cx = nullptr;
        return 0;
    }

    {
        JSAutoRealm ar(g_cx, global);
        if (!JS::InitRealmStandardClasses(g_cx) || !install_builtins(g_cx, global)) {
            JS_DestroyContext(g_cx);
            g_cx = nullptr;
            return 0;
        }
        /* Inside the realm: handling an interrupt dereferences cx->realm(). */
        discover_interrupt_bits(g_cx);
        if (g_bits_addr == nullptr) {
            /* Fallback: stay armed so the callback keeps being invoked. The
             * callback re-arms on every resume. */
            JS_RequestInterruptCallback(g_cx);
        }
    }

    g_global = new JS::PersistentRootedObject(g_cx, global);
    g_interrupt = 0;
    return 1; /* opaque handle */
}

std::string js_pump_jobs(uint64_t h) {
    if (!g_cx || h == 0) {
        g_stdout.clear();
        g_stderr.clear();
        return make_result(false, "", "no runtime");
    }
    g_stdout.clear();
    g_stderr.clear();

    JS::RootedObject global(g_cx, g_global->get());
    JSAutoRealm ar(g_cx, global);

    bool ran = host_timers_run(g_cx, &g_timers, /* shutting_down */ false);
    /* RunJobs drains microtasks AND the engine's internal dispatch queue —
     * a cross-thread waitAsync resolution lands there. It blocks while an
     * off-thread task is outstanding, which is the wait the host wants: the
     * notifying agent, or the engine's own timeout, releases it. */
    js::RunJobs(g_cx);
    if (JS_IsExceptionPending(g_cx)) {
        return make_result(false, "", take_error(g_cx));
    }
    bool progressed = ran || !g_stdout.empty() || !g_stderr.empty();
    /* Three-way result so the host's event loop can be exact instead of
     * guessing: "1" = work ran; "2" = nothing ran but work is PENDING (a
     * host timer not yet due, or an engine-delayed dispatchable — an
     * Atomics.waitAsync timeout — still queued), so wait and pump again;
     * "0" = nothing ran and nothing pending: the loop can stop. */
    bool pending = host_timers_pending(&g_timers);
#ifdef SPIDERMONKEY_WASM_THREADS
    if (!pending) {
        pending = js::Wasm2GoEarliestDelayedDispatchMs(g_cx) >= 0;
    }
#endif
    return make_result(true, progressed ? "1" : (pending ? "2" : "0"), "");
}

std::string js_eval(uint64_t h, const char *src, uint32_t src_len) {
    if (!g_cx || h == 0) {
        g_stdout.clear();
        g_stderr.clear();
        return make_result(false, "", "no runtime");
    }

    g_stdout.clear();
    g_stderr.clear();

    JS::RootedObject global(g_cx, g_global->get());
    JSAutoRealm ar(g_cx, global);

    /* Fallback mode: the host cannot trip interruptBits_ itself, so the engine
     * only polls while the interrupt is armed — and an interrupt that fires
     * consumes the arming (handleInterrupt clears the bits, and the terminating
     * path does not re-arm). Arm here so every eval is interruptible, not just
     * the first. In discovery mode the host arms it by storing the bit, and
     * arming here would only add a pointless trip through handleInterrupt. */
    if (g_bits_addr == nullptr) {
        JS_RequestInterruptCallback(g_cx);
    }

    JS::CompileOptions opts(g_cx);
    opts.setFileAndLine("<eval>", 1);

    /* Length-aware on purpose: JS source may legally contain NUL bytes (inside
     * string/template literals), so the byte count comes from the std::string,
     * never from strlen: the byte count is the caller's explicit src_len. */
    JS::SourceText<mozilla::Utf8Unit> buf;
    if (!buf.init(g_cx, src ? src : "", src ? src_len : 0, JS::SourceOwnership::Borrowed)) {
        JS_ClearPendingException(g_cx);
        return make_result(false, "", "could not read source");
    }

    JS::RootedValue rval(g_cx);
    bool ok = JS::Evaluate(g_cx, opts, buf, &rval);
    if (ok) {
        /* Run whatever microtasks the script queued. A job that throws leaves an
         * exception pending, which we surface exactly like a top-level throw. */
        js::RunJobs(g_cx);
        ok = !JS_IsExceptionPending(g_cx);
    }
    if (!ok) {
        return make_result(false, "", take_error(g_cx));
    }
    return make_result(true, to_utf8(g_cx, rval), "");
}

void js_close(uint64_t h) {
    if (!g_cx || h == 0) {
        return;
    }
#ifdef SPIDERMONKEY_WASM_THREADS
    /* Deterministic agent shutdown, then JOIN: an agent outliving this
     * runtime would touch a destroyed agent cluster (SAB waiter list,
     * off-thread promise state). shutdown+notify wakes agents parked on the
     * event futex; the urgent interrupt reaches ones parked inside the
     * engine (Atomics.wait, RunJobs' internal drain). */
    {
        g_agents.shutdown.store(true, std::memory_order_seq_cst);
        /* RE-SIGNAL until every agent has exited, parking on the event futex
         * (50 ms bound) between rounds. One shot is not enough: the
         * urgent interrupt only WAKES a wait in progress — fired in the window
         * between an agent's last loop-head check and its Atomics.wait
         * entry, it is recorded but wakes nothing, and the wait (infinite,
         * for a hostile guest) would never end. Each round re-fires the
         * idempotent interrupt, so an agent inside a wait is terminated by
         * the next round at the latest; the futex wait returns early the
         * moment any agent exits (they bump-and-notify on the way out). */
        while (g_agents.alive.load(std::memory_order_seq_cst) != 0) {
            {
                std::lock_guard<std::mutex> lock(g_agents.mu);
                for (JSContext *acx : g_agents.contexts) {
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
            uint32_t seen = g_agents.epoch.load(std::memory_order_seq_cst);
            if (g_agents.alive.load(std::memory_order_seq_cst) == 0) {
                break;
            }
            agent_event_wait(seen, 50 * 1000 * 1000);
        }
        std::vector<pthread_t> threads;
        {
            std::lock_guard<std::mutex> lock(g_agents.mu);
            threads = g_agents.threads;
        }
        for (pthread_t t : threads) {
            pthread_join(t, nullptr);
        }
        {
            std::lock_guard<std::mutex> lock(g_agents.mu);
            g_agents.threads.clear();
            g_agents.shutdown.store(false, std::memory_order_seq_cst);
            g_agents.broadcast_ready = false;
            g_agents.sab_clone.reset();
            g_agents.reports.clear();
        }
    }
#endif
    /* Persistent roots must be released while their runtime is still alive. */
    if (g_modules) {
        for (auto &entry : *g_modules) {
            delete entry.second.js;
            delete entry.second.json;
        }
        delete g_modules;
        g_modules = nullptr;
    }
    g_missing_modules.clear();
    delete g_global;
    g_global = nullptr;
    JS_DestroyContext(g_cx);
    g_cx = nullptr;
    g_bits_addr = nullptr;
    g_bits_value = 0;
    /* JS_ShutDown is intentionally NOT called here: it is process teardown, and
     * the instance may create a fresh runtime afterwards. */
}

uint32_t js_interrupt_addr(uint64_t h) {
    (void)h;
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_interrupt));
}

uint32_t js_interrupt_bits_addr(uint64_t h) {
    (void)h;
    return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(g_bits_addr));
}

uint32_t js_interrupt_bits_value(uint64_t h) {
    (void)h;
    return g_bits_value;
}
