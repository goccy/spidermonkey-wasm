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
static bool interrupt_cb(JSContext *cx) {
    if (g_discovering) {
        return true;
    }
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

static std::string make_result(bool ok, const std::string &result, const std::string &error) {
    std::string j = "{\"ok\":";
    j += ok ? "true" : "false";
    j += ",";
    j += json_field("result", result, true);
    j += json_field("stdout", g_stdout, true);
    j += json_field("stderr", g_stderr, true);
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

/* Registered modules, keyed by specifier. Values are heap-allocated
 * PersistentRooteds released in js_close (they must not outlive the runtime).
 * The registry IS the loader: the host resolves specifiers to sources and
 * registers them; the guest never does I/O (see js.h). */
static std::map<std::string, JS::PersistentRootedObject *> *g_modules = nullptr;

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

/* Compile src as a module, tag it with its specifier (the private value the
 * load hook reads back through GetScriptPrivate), and register it. */
static JSObject *compile_and_register_module(JSContext *cx, const std::string &specifier,
                                             const std::string &src) {
    JS::CompileOptions opts(cx);
    opts.setFileAndLine(specifier.c_str(), 1);
    JS::SourceText<mozilla::Utf8Unit> buf;
    if (!buf.init(cx, src.data(), src.size(), JS::SourceOwnership::Borrowed)) {
        return nullptr;
    }
    JS::RootedObject module(cx, JS::CompileModule(cx, opts, buf));
    if (!module) {
        return nullptr;
    }
    JS::RootedString specStr(cx, JS_NewStringCopyN(cx, specifier.data(), specifier.size()));
    if (!specStr) {
        return nullptr;
    }
    JS::SetModulePrivate(module, JS::StringValue(specStr));

    if (!g_modules) {
        g_modules = new std::map<std::string, JS::PersistentRootedObject *>();
    }
    auto it = g_modules->find(specifier);
    if (it != g_modules->end()) {
        delete it->second;
        g_modules->erase(it);
    }
    (*g_modules)[specifier] = new JS::PersistentRootedObject(cx, module);
    return module;
}

/* Look up the module a request resolves to, or report (and leave pending) a
 * "module not registered" error. */
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
            return it->second->get();
        }
    }
    JS_ReportErrorUTF8(cx, "module not registered: %s", resolved.c_str());
    return nullptr;
}

static bool load_module_resolved(JSContext *cx, JS::Handle<JS::Value> hostDefined) {
    (void)cx;
    (void)hostDefined;
    return true;
}

static bool load_module_rejected(JSContext *cx, JS::Handle<JS::Value> hostDefined,
                                 JS::Handle<JS::Value> error) {
    (void)hostDefined;
    /* Re-raise so the caller's take_error sees the real reason. */
    JS_SetPendingException(cx, error);
    return true;
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
        return JS::FinishLoadingImportedModule(cx, nullptr, moduleRequest, payload, module,
                                               /* usePromise = */ false);
    }
    return JS::FinishLoadingImportedModule(cx, referrer, moduleRequest, payload, module,
                                           /* usePromise = */ false);
}

std::string js_module_register(uint64_t h, const std::string &specifier,
                               const std::string &src) {
    if (!g_cx || h == 0) {
        g_stdout.clear();
        g_stderr.clear();
        return make_result(false, "", "no runtime");
    }
    g_stdout.clear();
    g_stderr.clear();
    JS::RootedObject global(g_cx, g_global->get());
    JSAutoRealm ar(g_cx, global);
    if (!compile_and_register_module(g_cx, specifier, src)) {
        return make_result(false, "", take_error(g_cx));
    }
    return make_result(true, "registered", "");
}

std::string js_eval_module(uint64_t h, const std::string &specifier,
                           const std::string &src) {
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
    }

    /* Promise jobs are queued and drained by us at the end of each js_eval; no
     * timers, no event loop, so an unsettled promise cannot hang the call. */
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

    if (!JS_AddInterruptCallback(g_cx, interrupt_cb)) {
        JS_DestroyContext(g_cx);
        g_cx = nullptr;
        return 0;
    }

    static JSClass global_class = {"global", JSCLASS_GLOBAL_FLAGS, &JS::DefaultGlobalClassOps};

    JS::RealmOptions options;
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

std::string js_eval(uint64_t h, const std::string &src) {
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
     * never from strlen. */
    JS::SourceText<mozilla::Utf8Unit> buf;
    if (!buf.init(g_cx, src.data(), src.size(), JS::SourceOwnership::Borrowed)) {
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
    /* Persistent roots must be released while their runtime is still alive. */
    if (g_modules) {
        for (auto &entry : *g_modules) {
            delete entry.second;
        }
        delete g_modules;
        g_modules = nullptr;
    }
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
