/* smoke.cc — a WASI command that exercises js.h end to end, without Go.
 *
 * Run it with scripts/run-smoke.sh. It links the same js.cc the shipped wasm
 * uses, so it covers the embedding — and above all the interrupt mechanism —
 * independently of wasmify, wasm2go and the Go bindings. When something breaks
 * after a SpiderMonkey bump, this says whether the engine layer or the Go layer
 * is at fault.
 *
 * The interrupt is normally tripped by the Go host writing two words into the
 * instance's linear memory. Those are plain stores, so a store performed from
 * inside the guest is indistinguishable from one performed by the host — which
 * is what lets this run under a stock `wasmtime run`. What it cannot cover is
 * concurrency (a host goroutine storing while the guest spins); that is a
 * property of the Go side and is tested there.
 *
 * Every assertion runs twice: once with the interrupt-bits discovery that js.cc
 * normally performs, and once with SPIDERMONKEY_WASM_NO_INTERRUPT_DISCOVERY set,
 * which forces the always-armed fallback. Both must pass — the fallback is a
 * slower path, not a weaker one.
 */
#include "js.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

/* js.cc reaches the host over two env imports (go_host_call/go_host_result):
 * the module loader and every host function travel over them. This standalone
 * harness has no host and exercises neither (it registers no host functions and
 * loads only import-free modules), but the module-resolve hook takes the
 * address of the loader, so the symbols are live and would otherwise remain
 * unsatisfied env imports that a stock `wasmtime run` cannot instantiate.
 * Define them locally as unreachable stubs so smoke.wasm self-contains. */
extern "C" uint32_t go_host_call(const char *, uint32_t, const char *, uint32_t,
                                 uint64_t, char *, uint32_t) {
    return 0;
}
extern "C" void go_host_result(char *) {}

static int failures = 0;

static void check(bool cond, const char *what) {
    std::printf("%s %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) failures++;
}

static bool contains(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
}

/* js_eval reports the completion value inside the envelope's "result" field as
 * an identity-preserving encoding, {"k":<kind>,"v":<data>}; in the envelope
 * string the encoding's own quotes are backslash-escaped. These build the
 * escaped needle for the primitive shapes the assertions below check. */
static bool result_num(const std::string &r, const char *n) {
    return contains(r, (std::string("{\\\"k\\\":\\\"number\\\",\\\"v\\\":") + n + "}").c_str());
}
static bool result_bool(const std::string &r, const char *b) {
    return contains(r, (std::string("{\\\"k\\\":\\\"bool\\\",\\\"v\\\":") + b + "}").c_str());
}
static bool result_str(const std::string &r, const std::string &s) {
    return contains(r, (std::string("{\\\"k\\\":\\\"string\\\",\\\"v\\\":\\\"") + s + "\\\"}").c_str());
}

/* Convenience overloads: the C API takes const char* + explicit length (the
 * wasmify bridge contract); tests call with std::string and the length rides
 * along automatically, embedded NULs included. */
static std::string js_eval(uint64_t h, const std::string &src) {
    return js_eval(h, src.c_str(), (uint32_t)src.size());
}
static std::string js_eval_module(uint64_t h, const std::string &spec, const std::string &src) {
    return js_eval_module(h, spec.c_str(), (uint32_t)spec.size(), src.c_str(), (uint32_t)src.size());
}

int main() {
    // When SPIDERMONKEY_WASM_NO_INTERRUPT_DISCOVERY is set, js.cc skips locating
    // JSContext::interruptBits_ and keeps the interrupt permanently armed
    // instead. Every interrupt assertion below must hold in BOTH modes: the
    // fallback is not a degraded mode, only a slower one.
    const bool fallback = std::getenv("SPIDERMONKEY_WASM_NO_INTERRUPT_DISCOVERY") != nullptr;
    std::printf("mode: %s\n", fallback ? "fallback (always-armed)" : "discovery");

    uint64_t h = js_new(64u * 1024 * 1024, 512u * 1024);
    check(h != 0, "js_new");
    if (!h) return 1;

    // --- discovery ---------------------------------------------------------
    const uint32_t bits_addr = js_interrupt_bits_addr(h);
    const uint32_t bits_val = js_interrupt_bits_value(h);
    std::printf("     interrupt_bits_addr=%#x value=%#x flag_addr=%#x\n", bits_addr, bits_val,
                js_interrupt_addr(h));
    if (fallback) {
        check(bits_addr == 0 && bits_val == 0, "fallback reports no interruptBits_ address");
    } else {
        check(bits_addr != 0, "discover_interrupt_bits located JSContext::interruptBits_");
        check(bits_val != 0 && (bits_val & (bits_val - 1)) == 0, "interrupt bit is a power of two");
    }

    // --- basic eval --------------------------------------------------------
    std::string r = js_eval(h, "1 + 2", sizeof("1 + 2") - 1);
    std::printf("     %s\n", r.c_str());
    check(contains(r, "\"ok\":true") && result_num(r, "3"), "eval arithmetic");

    r = js_eval(h, "globalThis.x = 41; x + 1", sizeof("globalThis.x = 41; x + 1") - 1);
    check(result_num(r, "42"), "eval global persists across calls");

    // The engine ships no I/O of its own: print and console are host surfaces
    // an embedder wires up (go-spidermonkey routes them to any io.Writer), not
    // engine builtins, and the eval envelope carries no stdout/stderr.
    r = js_eval(h, "typeof print + ',' + typeof console");
    check(result_str(r, "undefined,undefined"), "no print/console builtins");

    r = js_eval(h, "throw new Error('boom')", sizeof("throw new Error('boom')") - 1);
    check(contains(r, "\"ok\":false") && contains(r, "boom"), "uncaught exception reported");

    r = js_eval(h, "JSON.parse('{\"a\":[1,2]}').a.length");
    check(result_num(r, "2"), "standard classes available (JSON round-trips)");

    r = js_eval(h, "let p = 0; Promise.resolve(7).then(v => { p = v; }); 'queued'", sizeof("let p = 0; Promise.resolve(7).then(v => { p = v; }); 'queued'") - 1);
    check(contains(r, "\"ok\":true"), "promise job queued");
    r = js_eval(h, "p", sizeof("p") - 1);
    check(result_num(r, "7"), "promise job drained before return");

    // The bridge must expose no I/O surface at all.
    r = js_eval(h, "typeof fetch + ',' + typeof setTimeout + ',' + typeof read", sizeof("typeof fetch + ',' + typeof setTimeout + ',' + typeof read") - 1);
    check(contains(r, "undefined,undefined,undefined"), "no fetch/setTimeout/read builtins");

    // A NUL byte is a legal JS source character (here, inside a string
    // literal); the length-aware bridge must not truncate the script at it.
    r = js_eval(h, std::string("'a\0b'.length", 12));
    check(result_num(r, "3"), "source with embedded NUL is not truncated");

    // --- ES modules ---------------------------------------------------------
    // Import resolution is host-driven now: the embedder supplies a module
    // loader over the go_host_call channel (go-spidermonkey's SetModuleLoader,
    // where the static/dynamic import graph is tested). This standalone harness
    // has no host, so it only exercises modules that import nothing.
    r = js_eval_module(h, "bare.js", "globalThis.mod0 = 'bare';");
    check(contains(r, "\"ok\":true"), "module with no imports evaluates");
    r = js_eval(h, "mod0");
    check(result_str(r, "bare"), "module side effects reach the global");

    // Top-level await over an already-resolved promise completes synchronously
    // (the job queue drains before returning).
    r = js_eval_module(h, "tla.js",
                       "const v = await Promise.resolve('tla-ok'); globalThis.mod3 = v;");
    check(contains(r, "\"ok\":true"), "top-level await module settles");
    r = js_eval(h, "mod3");
    check(result_str(r, "tla-ok"), "top-level await value observed");

    // --- generic engine primitives -----------------------------------------
    // $262 is composed on the Go side now (go-spidermonkey's test262 host),
    // from the generic primitives js.h exposes — js_gc, js_new_realm,
    // js_eval_in, js_detach_array_buffer, js_new_htmldda — which are covered
    // there. Here we just confirm the engine survives a collection.
    js_gc(h);
    r = js_eval(h, "1 + 1");
    check(result_num(r, "2"), "engine evaluates after js_gc");

    // --- raw bytes bridge ----------------------------------------------------
    // js_bytes_new / js_bytes_read carry binary payloads with no encoding;
    // NULs and high bytes must survive both directions, and the created
    // handle must be a real script-visible Uint8Array.
    {
        const unsigned char raw[] = {0x00, 0x01, 0x7f, 0x80, 0xfe, 0xff};
        uint64_t bh = js_bytes_new(h, (const char *)raw, (uint32_t)sizeof raw);
        check(bh != 0, "js_bytes_new returns a handle");
        std::string bytes = js_bytes_read(h, bh);
        check(bytes.size() == 1 + sizeof raw && bytes[0] == 'B' &&
                  std::memcmp(bytes.data() + 1, raw, sizeof raw) == 0,
              "js_bytes_read round-trips NUL and high bytes");
        uint64_t g = js_global(h);
        const std::string enc = "{\"k\":\"object\",\"h\":" + std::to_string(bh) + "}";
        js_set(h, g, "hb", 2, enc.c_str(), (uint32_t)enc.size());
        r = js_eval(h, "hb instanceof Uint8Array && hb.join(',') === '0,1,127,128,254,255'");
        check(result_bool(r, "true"), "js_bytes_new array is a script-visible Uint8Array");
        std::string not_binary = js_bytes_read(h, g);
        check(!not_binary.empty() && not_binary[0] == 'E',
              "js_bytes_read rejects a non-binary object");
        js_free_object(bh);
        js_free_object(g);
        uint64_t empty = js_bytes_new(h, nullptr, 0);
        check(empty != 0 && js_bytes_read(h, empty) == "B",
              "zero-length payload round-trips as an empty Uint8Array");
        js_free_object(empty);
    }

    // Intl availability must match how the engine archive was built:
    // SPIDERMONKEY_WASM_EXPECT_INTL=1 for --with-intl-api builds (see
    // scripts/build-engine-intl.sh), unset for the --without-intl-api
    // StarlingMonkey prebuilt. Asserting both directions keeps the two
    // archive flavors from being swapped unnoticed.
    const bool expect_intl = std::getenv("SPIDERMONKEY_WASM_EXPECT_INTL") != nullptr;
    r = js_eval(h, "typeof Intl");
    check(expect_intl ? result_str(r, "object") : result_str(r, "undefined"),
          expect_intl ? "Intl is present (with-intl engine)" : "Intl is absent (without-intl engine)");
    if (expect_intl) {
        r = js_eval(h, "new Intl.NumberFormat('ja-JP').format(1234567)");
        std::printf("     %s\n", r.c_str());
        check(contains(r, "1,234,567"), "Intl.NumberFormat formats with locale data");
        r = js_eval(h, "/\\p{Script=Hiragana}/u.test('\xe3\x81\x82') ? 'prop-ok' : 'no'");
        check(contains(r, "prop-ok"), "regexp Unicode property escapes work");
        r = js_eval(h, "'\\u0041\\u030A'.normalize('NFC') === '\\u00C5' ? 'nfc-ok' : 'no'");
        check(contains(r, "nfc-ok"), "String.prototype.normalize works");
        // Informational: what else this build ships (Temporal is expected to
        // ride along with Intl in current SpiderMonkey).
        r = js_eval(h, "typeof Temporal + ',' + typeof Intl.Segmenter");
        std::printf("     extras: %s\n", r.c_str());

        // Temporal must WORK, not merely exist: date arithmetic exercises the
        // calendar code, and resolving a named time zone exercises the ICU
        // zoneinfo data compiled into the archive.
        r = js_eval(h, "Temporal.PlainDate.from('2026-07-11').add({days: 30}).toString()");
        check(contains(r, "2026-08-10"), "Temporal date arithmetic works");
        r = js_eval(h,
                    "Temporal.Instant.from('2026-07-11T00:00:00Z')"
                    ".toZonedDateTimeISO('Asia/Tokyo').hour");
        std::printf("     tz: %s\n", r.c_str());
        check(result_num(r, "9"), "Temporal named time zones resolve (ICU zoneinfo)");
        r = js_eval(h, "new Intl.Segmenter('ja', {granularity: 'word'})"
                       " && [...new Intl.Segmenter('ja', {granularity: 'word'})"
                       ".segment('今日は良い天気')].length > 1 ? 'segmenter-ok' : 'no'");
        check(contains(r, "segmenter-ok"), "Intl.Segmenter segments Japanese (ICU4X)");
        // Probe only — upstream SpiderMonkey does not ship ShadowRealm either.
        r = js_eval(h, "typeof ShadowRealm");
        std::printf("     ShadowRealm: %s\n", r.c_str());

        // Single-agent shared memory: SharedArrayBuffer and non-blocking
        // Atomics are spec-conformant without any threads. Blocking waits are
        // exercised with a zero timeout so neither legal outcome (immediate
        // "timed-out" where [[CanBlock]], TypeError where not) can hang.
        r = js_eval(h, "typeof SharedArrayBuffer + ',' + typeof Atomics");
        std::printf("     shared: %s\n", r.c_str());
        check(contains(r, "function,object"), "SharedArrayBuffer and Atomics exist");
        r = js_eval(h, "const sab = new SharedArrayBuffer(8); const ia = new Int32Array(sab);"
                       "Atomics.add(ia, 0, 41); Atomics.add(ia, 0, 1); Atomics.load(ia, 0)");
        check(result_num(r, "42"), "non-blocking Atomics work on a SharedArrayBuffer");
        r = js_eval(h, "try { 'wait:' + Atomics.wait(new Int32Array(new SharedArrayBuffer(8)), 0, 0, 0) }"
                       "catch (e) { 'threw:' + e.constructor.name }");
        std::printf("     %s\n", r.c_str());
        check(contains(r, "wait:timed-out") || contains(r, "threw:TypeError"),
              "Atomics.wait with zero timeout returns or throws, never hangs");
        // 64-bit atomics must WORK, not crash: the upstream feeling-lucky
        // arch allowlist predates wasm32 and MOZ_CRASHed the instance here
        // (a one-line guest-JS DoS) before the engine patch.
        r = js_eval(h, "Atomics.add(new BigInt64Array(new SharedArrayBuffer(16)), 0, 41n)"
                       " + ',' + Atomics.load(new BigInt64Array(new ArrayBuffer(16)), 0)");
        check(result_str(r, "0,0"), "64-bit Atomics work instead of crashing");
    }

    // --- interrupt: infinite loop -----------------------------------------
    // Exactly what the Go host's Interrupter.Fire() does, in the same order:
    // the "host asked" flag first, then the bit that trips SpiderMonkey's poll.
    auto fire = [&] {
        *reinterpret_cast<volatile uint32_t *>(js_interrupt_addr(h)) = 1;
        if (bits_addr) {
            *reinterpret_cast<volatile uint32_t *>(bits_addr) |= bits_val;
        }
    };

    fire();
    r = js_eval(h, "while (true) {}", sizeof("while (true) {}") - 1);
    std::printf("     %s\n", r.c_str());
    check(contains(r, "\"ok\":false") && contains(r, "interrupted"), "infinite loop interrupted");

    // The runtime must still be usable afterwards.
    r = js_eval(h, "1 + 1", sizeof("1 + 1") - 1);
    check(result_num(r, "2"), "runtime usable after interrupt");

    // --- interrupt is uncatchable -----------------------------------------
    // The whole security argument: guest JS must not be able to swallow it.
    fire();
    r = js_eval(h, "try { while (true) {} } catch (e) { 'swallowed' } finally { }", sizeof("try { while (true) {} } catch (e) { 'swallowed' } finally { }") - 1);
    std::printf("     %s\n", r.c_str());
    check(contains(r, "\"ok\":false") && !contains(r, "swallowed"),
          "interrupt is uncatchable by try/catch");

    r = js_eval(h, "2 * 3", sizeof("2 * 3") - 1);
    check(result_num(r, "6"), "runtime usable after uncatchable interrupt");

    // Regression: an interrupt that fires consumes SpiderMonkey's armed state
    // (handleInterrupt clears interruptBits_ before calling us). If nothing
    // re-arms, the SECOND interrupt on the same runtime never fires and this
    // eval hangs forever. That is only visible from the third interrupt onward
    // in fallback mode, so exercise it repeatedly.
    for (int i = 0; i < 3; i++) {
        fire();
        r = js_eval(h, "while (true) {}", sizeof("while (true) {}") - 1);
        check(contains(r, "\"ok\":false") && contains(r, "interrupted"),
              "repeated interrupt still fires (re-armed)");
    }

    // --- an interrupt that was never requested must not fire ---------------
    r = js_eval(h, "let n = 0; for (let i = 0; i < 3000000; i++) n += i; n > 0", sizeof("let n = 0; for (let i = 0; i < 3000000; i++) n += i; n > 0") - 1);
    check(contains(r, "\"ok\":true") && result_bool(r, "true"),
          "long loop completes when no interrupt is pending");

    // --- recursion is bounded, not a wasm trap -----------------------------
    r = js_eval(h, "function f(){ return f(); } try { f() } catch (e) { 'caught:' + e.name }", sizeof("function f(){ return f(); } try { f() } catch (e) { 'caught:' + e.name }") - 1);
    std::printf("     %s\n", r.c_str());
    check(contains(r, "caught:InternalError") || contains(r, "too much recursion"),
          "native stack quota turns runaway recursion into a JS error");

    // --- heap cap ----------------------------------------------------------
    // Report what was actually thrown: an assertion that merely checks the loop
    // stopped would also pass if some unrelated error ended it.
    {
        const char heapSrc[] =
            "let a = [];\n"
            "try { for(;;) a.push(new Array(100000).fill(0)); a = null; 'never' }\n"
            "catch (e) { a = null; 'threw:' + String(e) }";
        r = js_eval(h, heapSrc, sizeof(heapSrc) - 1);
    }
    std::printf("     %s\n", r.c_str());
    check(contains(r, "out of memory") || contains(r, "allocation size overflow") ||
              (contains(r, "\"ok\":false") && contains(r, "memory")),
          "heap cap stops runaway allocation with an out-of-memory error");

    js_close(h);
    check(true, "js_close");

    // --- scaled recursion ceiling (source-built engines only) ---------------
    // Engines from scripts/build-engine-intl.sh carry the mutable wasi
    // recursion-limit patch: js_new scales the depth ceiling with the stack
    // quota. 60 nested function literals exceed the upstream fixed ceiling
    // (parse recursion hits it at ~29 with the 350-unit constant) and must
    // parse once a 4 MiB quota raises it.
    if (expect_intl) {
        h = js_new(64u * 1024 * 1024, 4u * 1024 * 1024);
        check(h != 0, "js_new with a 4 MiB quota");
        std::string deep;
        for (int i = 0; i < 60; i++) deep += "(function(){return ";
        deep += "1";
        for (int i = 0; i < 60; i++) deep += "})()";
        r = js_eval(h, deep.c_str());
        std::printf("     deep-nest: %.120s\n", r.c_str());
        check(result_num(r, "1"), "recursion ceiling scales with the stack quota");
        js_close(h);
    }

    std::printf("\n%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
