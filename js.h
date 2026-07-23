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
 * crosses the bridge intact.
 *
 * The runtime handle IS the runtime: js_new heap-allocates the per-runtime
 * state (context, global, capture buffers, module registry, agent cluster,
 * ...) and returns its address as an opaque uint64. There are NO process
 * globals for per-runtime state — every export takes the handle, and natives
 * reach the runtime through their context's private slot — so several
 * runtimes can coexist in one instance. The uint64 (rather than a
 * pointer-to-opaque-struct parameter) also keeps the bridge generator
 * unambiguous and is the conventional FFI handle idiom.
 *
 * Threading model: the embedding drives a runtime from one thread at a time
 * (agents spawned by js_agent_spawn run on their own threads with their own
 * contexts). SpiderMonkey helper-thread work is dispatched to host threads in
 * threads builds.
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
 * than growing wasm linear memory. Zero means uncapped on the JS side — the
 * host-side wasm memory cap (Config.MaxMemoryBytes) is then the single
 * effective limit, which is the supported configuration.
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
 * "result" holds the script's completion value as a VALUE ENCODING (see the
 * object-handle bindings below): a primitive carries its data and type, an
 * object or function carries a persistent handle, so identity survives the
 * bridge. Valid only when "ok" is true. "stdout"/"stderr" hold anything the
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

/* One step of the host event loop: run due host timers, then js::RunJobs —
 * the engine's own drain of the job queue (ECMA-262 Jobs, §9.5: microtasks
 * plus cross-thread Dispatchables another agent queued, e.g. Atomics.waitAsync
 * resolutions). This is deliberately the ONLY pre/post-processing bundled in:
 * the timer store and the pending-work probe live in C++ (the timers are this
 * bridge's own state; the probe needs engine internals), so every conceivable
 * host loop would have to do exactly these steps around js::RunJobs. Loop
 * POLICY — when to stop, how long to wait — stays host-side.
 *
 * Returns the same {ok, result, error} envelope as js_eval; result is "1" if
 * the step made progress (output was produced or a job ran), "2" if nothing
 * ran but work is still pending (a timer not yet due, or an engine-delayed
 * Atomics.waitAsync timeout) — wait briefly and call again — and "0" if
 * nothing ran and nothing is pending, so the loop can stop. stdout/stderr
 * produced by the drained jobs is captured exactly like js_eval's. */
std::string js_run_jobs(uint64_t h);

/* ---- ES modules ------------------------------------------------------------
 *
 * Module loading is a LOADER CALLBACK: an import that misses the per-runtime
 * cache asks the host for source through the reserved go_host_call key
 * "\0module-load" (args [resolved specifier, referrer]; reply 'R' + raw
 * source or 'E' + message). The engine resolves ./ and ../ against the
 * importing module's specifier before asking; sources are cached and compiled
 * lazily per import, so a compile error surfaces at import time with its real
 * type, and one source can serve as a JS module or — with
 * `with { type: "json" }` — as a JSON module. With no loader attached
 * (total == 0), every import fails "module not registered: <specifier>"
 * (sandbox default-deny). */

/* Compile `src` as an ES module registered under `specifier`, load its
 * dependency graph (through the loader), link, evaluate, and drain the job
 * queue. Same JSON shape as js_eval: "ok" true when the module (including
 * top-level await) evaluated to completion; "error" carries
 * compile/link/import/runtime failures. */
std::string js_eval_module(uint64_t h, const char *specifier, uint32_t specifier_len,
                           const char *src, uint32_t src_len);

/* ---- raw object-handle JSAPI bindings --------------------------------------
 * The internal (go-spidermonkey/internal) raw layer that a public embedding API
 * is built on. An object handle is a uint64 = JS::PersistentRooted<JSObject*>*
 * (a GC-stable cell); js_free_object releases it, driven by a Go finalizer. */

/* The current global object as a handle. */
uint64_t js_global(uint64_t h);

/* A fresh plain object (JS_NewPlainObject) as a handle. */
uint64_t js_new_plain_object(uint64_t h);

/* Define a host function on obj_handle — the deliberate host-surface opt-in:
 * the sandbox exposes nothing until the embedder defines a function. Calls
 * dispatch to the Go function the embedder registered under `key` (arguments
 * as a JSON array of value encodings; reply 'R' + one encoding or 'E' +
 * message); `name` is the property name on the object. */
std::string js_define_function(uint64_t h, uint64_t obj_handle, const char *name,
                               uint32_t name_len, const char *key, uint32_t key_len,
                               uint32_t nargs);

/* Define a CONSTRUCTABLE host function on obj_handle — like js_define_function
 * but `new name(...)` is allowed, so a real host class (e.g. `new Worker(...)`)
 * can be defined from the host. The Go function's returned object becomes the
 * instance. */
std::string js_define_constructor(uint64_t h, uint64_t obj_handle, const char *name,
                                  uint32_t name_len, const char *key, uint32_t key_len,
                                  uint32_t nargs);

/* Release an object handle (delete its persistent root). */
void js_free_object(uint64_t obj_handle);

/* Get obj_handle[name] as a value encoding (primitive data, or an object/
 * function handle) preserving identity — not a stringification. */
std::string js_get(uint64_t h, uint64_t obj_handle, const char *name, uint32_t name_len);

/* Set obj_handle[name] to the decoded value encoding `val` (a primitive, an
 * object/function by handle, or {"k":"json","v":<data>} — host composite data
 * that materializes as a fresh guest Array/Object). Returns {"k":"undefined"}
 * on success, or an error encoding. */
std::string js_set(uint64_t h, uint64_t obj_handle, const char *name, uint32_t name_len,
                   const char *val, uint32_t val_len);

/* Call callable fn_handle with this_handle (0 = undefined) and args (a JSON
 * array of value encodings); return the result as a value encoding. */
std::string js_call(uint64_t h, uint64_t fn_handle, uint64_t this_handle, const char *args,
                    uint32_t args_len);

/* A fresh host-backed FUNCTION object, attached to nothing: calling it from JS
 * dispatches to the Go function the embedder registered under `key`, exactly
 * like a js_define_function stub, but the function is returned as a handle
 * instead of being defined as a property. This is the Go-side FuncOf: the
 * embedder composes it into any structure (a callback argument, an
 * underlyingSource.pull, an object method) via js_set / js_call /
 * js_construct. Returns 0 on failure. */
uint64_t js_new_function(uint64_t h, const char *name, uint32_t name_len, const char *key,
                         uint32_t key_len, uint32_t nargs);

/* Construct `new fn(...args)` — the [[Construct]] counterpart of js_call
 * (fn_handle must be a constructor: a class or function). args is the same
 * JSON array of value encodings; the return is the new instance's value
 * encoding ({"k":"error",...} when construction threw). */
std::string js_construct(uint64_t h, uint64_t fn_handle, const char *args, uint32_t args_len);

/* ---- raw bytes -------------------------------------------------------------
 * Binary data crosses the bridge RAW: the generated protobuf channel is
 * length-delimited and 8-bit clean in both directions (explicit lengths,
 * never strlen), so bytes — including NULs and non-UTF-8 sequences — need no
 * base64/JSON encoding. These two functions are the []byte <-> Uint8Array
 * fast path a host binary API is built on. */

/* Create a fresh Uint8Array of data_len bytes initialized with a copy of
 * `data`, and return it as an object handle (0 on failure). The copy happens
 * inside this call, so no engine data pointer ever crosses the bridge. */
uint64_t js_bytes_new(uint64_t h, const char *data, uint32_t data_len);

/* Copy the binary contents of obj_handle out of the engine: a Uint8Array or
 * any other ArrayBuffer view (read as its raw bytes, honoring offset/length),
 * an ArrayBuffer, or a SharedArrayBuffer. Returns 'B' + the bytes on success,
 * or 'E' + message when the object is not binary (the one-byte tag
 * disambiguates an empty buffer from an error). */
std::string js_bytes_read(uint64_t h, uint64_t obj_handle);

/* ---- agents ----------------------------------------------------------------
 *
 * ECMA-262 specifies what an agent IS (its own thread of execution and realm,
 * sharing nothing with other agents but SharedArrayBuffer memory — one agent
 * cluster per process here) and leaves creation/communication to the host.
 * The bridge keeps ONLY what physically cannot leave C++ — the thread
 * mechanics (a JSContext is single-threaded, so the agent's context, source
 * evaluation and job pump run on its own thread) and structured clone
 * write/read (which runs on the thread owning the value's context). ALL
 * communication policy — queues, broadcast latching, routing, lifecycle —
 * lives host-side: the agent's primitives are forwarded to the host through
 * reserved go_host_call keys ("\0agent-receive", "\0agent-post",
 * "\0agent-exit"; NUL-prefixed like the module loader's), and the host may
 * block an agent's receive as long as it wants. $262.agent, Web Workers and
 * Node worker_threads are adapters the embedder composes on top.
 *
 * Values cross agents as STRUCTURED CLONES with shared-memory objects allowed
 * (the spec route): a cloned SharedArrayBuffer shares the SAME memory,
 * everything else is deep-copied data. Between threads a clone travels as a
 * CLONE HANDLE the host owns.
 *
 * Threads builds only; the single-agent build stubs spawn (returns 0). */

/* Spawn a new agent evaluating src on its own thread/context/global. The
 * agent's global sees the standard classes, print/console, and the RAW host
 * channels — no policy:
 *   __agent_call__(op, extra?)  one reserved-key round trip ("\0" + op, op
 *                               must start with "agent-"); the agent id is
 *                               injected as the first argument; the host may
 *                               block this goroutine (that is how a receive
 *                               waits). Returns tag+payload as a string.
 *   __clone_read__(handle)      deserialize a host-owned clone here
 *   __clone_write__(v)          clone v here; returns the handle
 *   __agent_leaving__()         mark done; the agent exits once idle
 * `glue` (trusted adapter setup composing its agent API — $262.agent, a Worker
 * scope, ... — from those natives) and `src` (the user source) are evaluated
 * as SEPARATE scripts, glue first: NEVER concatenated, so the user source keeps
 * its own "use strict", line numbers and directive prologue, and can even be a
 * module. After both evaluate, the agent keeps draining its job queue until
 * leaving or runtime close; on exit the skeleton sends "\0agent-exit".
 * Returns an opaque non-zero agent id, or 0 on failure. */
uint64_t js_agent_spawn(uint64_t h, const char *glue, uint32_t glue_len, const char *src,
                        uint32_t src_len);

/* Wake every agent pump parked on the event futex, so an agent whose inbox the
 * host just filled (Send) delivers promptly. Safe to call at any time. */
void js_agent_wake(uint64_t h);

/* Clone the decoded value encoding on the MAIN thread into a clone handle
 * (0 = not clonable). The handle is owned by the caller: hand it to an agent
 * (reply to "\0agent-receive") or free it with js_clone_free. */
uint64_t js_clone_write(uint64_t h, const char *val, uint32_t val_len);

/* Deserialize a clone handle into the MAIN runtime; returns the value
 * encoding. The handle stays valid — one broadcast clone can be read by many
 * receivers — until js_clone_free. */
std::string js_clone_read(uint64_t h, uint64_t clone_handle);

/* Release a clone handle. */
void js_clone_free(uint64_t clone_handle);

/* ---- realm / engine primitives ----------------------------------------------
 * Generic JSAPI bindings; a conformance harness ($262) or any embedder
 * composes its surface from these host-side. Nothing harness-shaped lives in
 * the engine bridge. */

/* Force a full garbage collection. */
void js_gc(uint64_t h);

/* Detach an ArrayBuffer object. Returns {"k":"undefined"} or an error
 * encoding. */
std::string js_detach_array_buffer(uint64_t h, uint64_t obj_handle);

/* A fresh SAME-COMPARTMENT realm (objects flow between realms directly) with
 * the standard classes and an EMPTY host surface; returns its global object
 * as a handle. js_define_function / js_set / js_get / js_call enter the
 * target object's realm, so composing the new realm works like composing the
 * main one. */
uint64_t js_new_realm(uint64_t h);

/* Evaluate src as a classic script in the realm of global_handle and return
 * the completion value as a value encoding ({"k":"error",...} on throw). The
 * raw synchronous evaluation primitive: it does NOT drain the job queue. */
std::string js_eval_in(uint64_t h, uint64_t global_handle, const char *src,
                       uint32_t src_len);

/* A fresh [[IsHTMLDDA]] object (emulates undefined, yields null when called
 * — document.all semantics; the class flag is engine-level), as a handle. */
uint64_t js_new_htmldda(uint64_t h);

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
