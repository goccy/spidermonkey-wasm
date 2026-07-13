/* js.h — thin SpiderMonkey embedding API exported to wasm / Go.
 *
 * This is the ONLY surface wasmify exports from libspidermonkey. SpiderMonkey's
 * full JSAPI stays internal; Go callers see just these functions. Pinned against
 * the SpiderMonkey the `starlingmonkey` submodule pins (see
 * scripts/fetch-spidermonkey.sh), currently Firefox 147, built for wasm32-wasi
 * with --disable-jit --enable-portable-baseline-interp.
 *
 * It is a C++ header (compiled into the wasmify bridge as C++): string OUTPUTS
 * use `std::string`, matching the bridge generator's string-output handling, and
 * string INPUTS use `const char*` (the bridge passes `.c_str()`) plus an
 * EXPLICIT length parameter — never strlen — so a script containing an
 * embedded NUL byte (legal in JS source, e.g. inside a string literal)
 * crosses the bridge intact. The runtime handle is an opaque
 * integer token (uint64), which keeps the generator unambiguous (a
 * pointer-to-opaque-struct parameter is otherwise misread as an output param)
 * and is the conventional FFI handle idiom.
 *
 * Threading model: one wasm instance == one JSContext == one global == one
 * handle. Multiple runtimes == multiple wasm2go module instances, each with its
 * own linear memory. SpiderMonkey is built single-threaded for wasi
 * (-mthread-model single); there are no helper threads.
 */
#ifndef SPIDERMONKEY_WASM_JS_H
#define SPIDERMONKEY_WASM_JS_H

#include <cstdint>
#include <string>

/* Initialize a JS runtime and return an opaque handle (0 on failure). Call once
 * per wasm instance.
 *
 * Internally: JS_Init (once per process), JS_NewContext, InitSelfHostedCode, a
 * fresh global with the standard classes, and the interrupt plumbing described
 * below.
 *
 * `max_heap_bytes`, when non-zero, caps the GC heap (JSGC_MAX_BYTES): an
 * allocation past it fails with an out-of-memory error inside the guest rather
 * than growing wasm linear memory. It is the JS-visible half of the sandbox;
 * the host-side wasm memory cap (Config.MaxMemoryBytes) is the backstop.
 *
 * `native_stack_quota_bytes`, when non-zero, caps native recursion depth
 * (JS_SetNativeStackQuota) so runaway recursion raises a catchable
 * "too much recursion" error instead of overflowing the wasm C stack, which
 * would trap the whole instance. Keep it comfortably below the linker's
 * -Wl,-z,stack-size. */
uint64_t js_new(uint32_t max_heap_bytes, uint32_t native_stack_quota_bytes);

/* Evaluate `src` as a classic script in the runtime's persistent global and
 * return the result as a JSON string:
 *
 *   {"ok":<bool>,"result":<string>,"stdout":<string>,"stderr":<string>,
 *    "error":<string>}
 *
 * "result" holds the stringification (ToString) of the script's completion
 * value, valid only when "ok" is true. "stdout"/"stderr" hold anything the
 * script wrote through the `print()` / `console.log()` / `console.error()`
 * functions this bridge installs — SpiderMonkey itself has no I/O, and this
 * bridge deliberately exposes no file, network, or timer builtins.
 * Global state persists across calls on the same handle (REPL-like).
 *
 * On an uncaught JS exception, "ok" is false and "error" holds the exception's
 * stringification followed by its stack. On a host interrupt (see below), "ok"
 * is false and "error" is "JS execution interrupted".
 *
 * Promise jobs queued by the script are drained before returning, so a
 * top-level `Promise.resolve().then(...)` runs. The bridge installs no timers,
 * so a job that never settles cannot block: there is nothing to wait on.
 *
 * A single JSON string return is used because the bridge generator surfaces
 * only one response value to Go; bundling the outputs keeps one round-trip and
 * one atomic result. The Go wrapper unmarshals it. */
std::string js_eval(uint64_t h, const char *src, uint32_t src_len);

/* Drain the runtime's job queue once (microtasks plus any cross-thread
 * Dispatchables another agent has queued — Atomics.waitAsync resolutions
 * arrive this way). Returns the same {ok, result, error} envelope as js_eval;
 * result is "1" if the pump made progress (output was produced or a job ran),
 * "2" if nothing ran but work is still pending (a timer not yet due, or an
 * engine-delayed Atomics.waitAsync timeout) — wait briefly and pump again —
 * and "0" if nothing ran and nothing is pending, so the loop can stop.
 * stdout/stderr produced by the drained jobs is captured exactly like
 * js_eval's. The host loops on this to run an event loop; the engine has no
 * loop of its own. */
std::string js_pump_jobs(uint64_t h);

/* ---- ES modules ------------------------------------------------------------
 *
 * The guest cannot call out to the host mid-link, so module loading is a
 * REGISTRY: the host (Go) resolves specifiers to sources however it likes —
 * filesystem, embedded maps, network, policy checks all live host-side — and
 * registers them here before (or between) evaluations. The guest only does
 * exact-match lookup plus ./ and ../ resolution against the importing module's
 * specifier. An import that misses the registry fails with
 * "module not registered: <resolved specifier>", which is the host's cue to
 * fetch, register, and retry. Nothing is registered by default: with no
 * registrations, every import fails (sandbox default-deny). */

/* Register `src` under `specifier`. Compilation is DEFERRED to the first
 * import that resolves to it: a compile error then rejects that import with
 * its real type (a dynamic import of script-only source rejects with
 * SyntaxError, per HostLoadImportedModule), and the same source can load as a
 * JS module or — when the import carries `with { type: "json" }` — as a JSON
 * module. Re-registering a specifier replaces the source (affects future
 * lookups only).
 *
 * When an import misses the registry, the failure carries the resolved
 * specifier in the error text AND every result JSON gains a
 * "missing_modules":[...] array — the latter is the only channel when guest
 * code catches a dynamic import rejection itself. The host loader's protocol
 * is: fetch, register, re-run. */
std::string js_module_register(uint64_t h, const char *specifier, uint32_t specifier_len,
                               const char *src, uint32_t src_len);

/* Compile `src` as an ES module registered under `specifier`, load its
 * dependency graph from the registry, link, evaluate, and drain the job queue.
 * Same JSON shape as js_eval: "ok" true when the module (including top-level
 * await) evaluated to completion; "error" carries compile/link/import/runtime
 * failures, or "module not registered: X" when an import misses the registry. */
std::string js_eval_module(uint64_t h, const char *specifier, uint32_t specifier_len,
                           const char *src, uint32_t src_len);

/* Install the $262 test-support object (https://github.com/tc39/test262
 * INTERPRETING.md) on this runtime's global: createRealm (same-compartment
 * realm with its own $262), detachArrayBuffer, evalScript, gc, global, and an
 * IsHTMLDDA object ([[IsHTMLDDA]], i.e. document.all emulation). NOT part of
 * the sandbox surface — call it only from conformance harnesses. */
void js_install_test262_hooks(uint64_t h);

/* Destroy the runtime (JS_DestroyContext). JS_ShutDown runs at process
 * teardown, not here, so the handle is fully torn down but the process stays
 * usable. */
void js_close(uint64_t h);

/* ---- Interruption support ------------------------------------------------
 *
 * Lets a host watchdog goroutine abort a runaway script (e.g. `while(1){}`)
 * WITHOUT executing any wasm/C code on that instance — which would corrupt the
 * shared linear-memory C stack. The host performs plain 32-bit stores into
 * linear memory; the guest notices at the next bytecode loop head.
 *
 * SpiderMonkey's own mechanism already has exactly this shape. JSContext holds
 * an `interruptBits_` word; `JS_RequestInterruptCallback` does nothing but a
 * relaxed atomic store into it, and the interpreter polls it at every loop head
 * (`JSOp::LoopHead` in PortableBaselineInterpret.cpp, `CHECK_BRANCH()` in
 * Interpreter.cpp). When set, the engine calls the registered interrupt
 * callback; a callback returning false terminates the script with an
 * UNCATCHABLE exception — guest JS cannot swallow it with
 * `try { while(1){} } catch {}`. That is a stronger guarantee than Perl's
 * croak, which a `eval {}` can catch.
 *
 * We cannot call JS_RequestInterruptCallback from the host: it is guest code,
 * and running it on another goroutine would clobber the instance's C stack
 * pointer. So the host writes the two words directly, IN THIS ORDER:
 *
 *   *(uint32_t *)js_interrupt_addr(h)       = 1;               // "the host asked"
 *   *(uint32_t *)js_interrupt_bits_addr(h) |= js_interrupt_bits_value(h);
 *
 * Both stores are needed and they mean different things. The second trips
 * SpiderMonkey's poll, so the engine calls our callback at the next loop head.
 * The first tells that callback the interrupt is ours rather than one the engine
 * raised for its own reasons; terminating a script on one of those would be a
 * spurious abort. The callback clears the first flag when it fires.
 *
 * The order matters because the guest reads the two words with independent,
 * unordered loads on another thread. Storing the flag first means a guest that
 * sees the bit will, at worst, see the flag one loop head later — the callback
 * re-arms and resumes when the flag is not yet visible, so a late flag costs a
 * few bytecodes rather than losing the interrupt. Storing the bit first would
 * make the reverse window lose it outright: the callback would resume, the
 * engine would have cleared the bit, and Eval would run forever while the host
 * believed it had cancelled the script.
 *
 * js_interrupt_bits_addr returns 0 when the address of `interruptBits_` could
 * not be established (it lives in SpiderMonkey's internal JSContext, which the
 * public headers do not describe; js.cc locates it at startup by probing, see
 * discover_interrupt_bits). The runtime then falls back to keeping the
 * interrupt permanently armed, which costs a trip through handleInterrupt at
 * every loop head but needs only the first store. Callers must therefore treat
 * a 0 from js_interrupt_bits_addr as "skip the second store", not as an error.
 *
 * Like Perl's opcode loop and CPython's eval-breaker, this only fires at
 * bytecode loop heads: a single long-running operation (a pathological regex, a
 * huge sort) is not preempted until it returns to the interpreter loop.
 *
 * Addresses are 32-bit linear-memory offsets (wasm32). */
uint32_t js_interrupt_addr(uint64_t h);       /* &host-asked flag (store 1 to trip) */
uint32_t js_interrupt_bits_addr(uint64_t h);  /* &JSContext::interruptBits_, or 0 */
uint32_t js_interrupt_bits_value(uint64_t h); /* bit to OR into that word, or 0 */

#endif /* SPIDERMONKEY_WASM_JS_H */
