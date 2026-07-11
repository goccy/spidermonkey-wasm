# spidermonkey-wasm

SpiderMonkey — Firefox's JavaScript engine — built for `wasm32-wasi`.

This repository holds the wasm side: the embedding layer (`js.h` / `js.cc`), the
[wasmify](https://github.com/goccy/wasmify) configuration that links it, and the
tests that keep the sandbox honest. It is the SpiderMonkey counterpart of
[`python-wasm`](https://github.com/goccy/python-wasm) and
[`perl-wasm`](https://github.com/goccy/perl-wasm).

## The engine is not built here

Building SpiderMonkey from mozilla-central takes hours and a full Gecko
toolchain. The [StarlingMonkey](https://github.com/bytecodealliance/StarlingMonkey)
project already publishes the wasm32-wasi build as a prebuilt
`libspidermonkey.a` plus public headers, compiled with `--disable-jit
--enable-portable-baseline-interp`. The `starlingmonkey` submodule pins the
engine version; `scripts/fetch-spidermonkey.sh` reads that pin and downloads the
matching archive.

Nothing else from StarlingMonkey is used. Its runtime is a WASI 0.2 component
with `fetch`, WHATWG Streams and an event loop; we want a WASI preview-1 core
module with none of that — every builtin is attack surface, and `wasm2go`
consumes preview-1 core modules.

The archive is not self-contained. SpiderMonkey's string conversions live in the
`encoding_rs` Rust crate, reached through `encoding_c` / `encoding_c_mem`, so
`rust/` bundles those into a staticlib, taken from the same submodule that pins
the engine because the two must agree on an ABI.

## The API

`js.h` is the entire surface:

```c
uint64_t    js_new(uint32_t max_heap_bytes, uint32_t native_stack_quota_bytes);
std::string js_eval(uint64_t h, const char *src);
void        js_close(uint64_t h);

uint32_t    js_interrupt_addr(uint64_t h);
uint32_t    js_interrupt_bits_addr(uint64_t h);
uint32_t    js_interrupt_bits_value(uint64_t h);
```

`js_eval` returns a JSON document — `{"ok", "result", "stdout", "stderr",
"error"}` — because the bridge generator surfaces a single response value to Go.
Global state persists across calls on the same handle, so it behaves like a REPL.

## Security

A guest script gets `print()` and `console.log/info/warn/error`, and nothing
else. There is no `fetch`, no timers, no file or network access: SpiderMonkey has
no I/O of its own, `js.cc` installs no builtin that reaches any, and the wasm is
linked with wasmify's host-sockets and host-subprocess capabilities off. Every
import in the module is `wasi_snapshot_preview1`.

Three limits bound what a script can consume.

**Time.** A host watchdog can abort a runaway script — `while (true) {}` —
*without executing any code on the instance*, which matters because running guest
code on another thread would corrupt its C stack. SpiderMonkey already has the
right shape for this: `JS_RequestInterruptCallback` does nothing but store into
`JSContext`'s `interruptBits_`, and the interpreter polls that word at every
bytecode loop head. So the host writes the word itself.

Its address is not public, so `js.cc` locates it at startup by requesting an
interrupt twice with two different reasons and diffing `JSContext`'s memory (see
`discover_interrupt_bits`). If the result is ambiguous it falls back to keeping
the interrupt permanently armed, which costs about 20% on a tight loop but is
otherwise identical.

The termination is an **uncatchable** exception: a script cannot swallow it with
`try { while (true) {} } catch (e) {}`. That is a stronger guarantee than the
Perl embedding gets, where an `eval {}` catches the `croak`.

**Memory.** `max_heap_bytes` caps the GC heap. A script that allocates past it
gets a catchable `out of memory` and the runtime survives. The wasm module's own
memory cap — `MaxMemoryBytes` on the Go side — is a *backstop that protects the
host*, not a limit the guest recovers from: several SpiderMonkey allocation paths
are infallible and abort rather than throw, so hitting the wasm cap traps the
instance. The heap cap must stay well below it; go-spidermonkey enforces a 4:1
ratio, which is where the measured boundary sits.

**Stack.** `native_stack_quota_bytes` turns runaway recursion into a catchable
`InternalError: too much recursion` instead of a C-stack overflow that would trap
the instance.

## Building

```sh
make wasm    # the whole pipeline, exactly as CI runs it
make smoke   # run tests/smoke.cc under wasmtime, in both interrupt modes
make help    # every target
```

`make wasm` fetches the engine and builds the Rust staticlib (`make deps`), parses
`js.h`, generates the proto and the C++ dispatcher, links `build/spidermonkey.wasm`,
transpiles it to Go under `build/wasm2go/`, and stamps the bundle's `go.mod`. The
link is a ThinLTO pass over the whole engine, so budget about a minute and several
gigabytes of RSS.

Everything the pipeline emits is git-ignored and regenerated: `api-spec.json`,
`proto/spidermonkey.proto`, `bridge/`, `build/`. `build.json` is committed rather
than generated — there is no native build to capture, so nothing regenerates it.
`make wasm-clean` drops the generated set and keeps the committed inputs and the
520 MB `deps/` tree.

Unlike python-wasm, this pipeline runs on the host rather than inside
`ghcr.io/goccy/wasmify`: the image carries no Rust toolchain, and the engine
arrives prebuilt, so there is no heavy native build for a container to cache.

`make smoke` is the fastest way to tell whether a SpiderMonkey bump broke the
engine layer or the Go layer: it links the same `js.cc` the shipped wasm uses and
drives it with no wasmify, wasm2go or Go involved.
