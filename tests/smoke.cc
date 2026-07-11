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
#include <string>

static int failures = 0;

static void check(bool cond, const char *what) {
    std::printf("%s %s\n", cond ? "ok  " : "FAIL", what);
    if (!cond) failures++;
}

static bool contains(const std::string &hay, const char *needle) {
    return hay.find(needle) != std::string::npos;
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
    std::string r = js_eval(h, "1 + 2");
    std::printf("     %s\n", r.c_str());
    check(contains(r, "\"ok\":true") && contains(r, "\"result\":\"3\""), "eval arithmetic");

    r = js_eval(h, "globalThis.x = 41; x + 1");
    check(contains(r, "\"result\":\"42\""), "eval global persists across calls");

    r = js_eval(h, "print('hello'); console.error('bad'); 'done'");
    std::printf("     %s\n", r.c_str());
    check(contains(r, "\"stdout\":\"hello\\n\"") && contains(r, "\"stderr\":\"bad\\n\""),
          "print/console captured");

    r = js_eval(h, "throw new Error('boom')");
    check(contains(r, "\"ok\":false") && contains(r, "boom"), "uncaught exception reported");

    r = js_eval(h, "JSON.stringify({a:[1,2]})");
    check(contains(r, "{\\\"a\\\":[1,2]}"), "standard classes available");

    r = js_eval(h, "let p = 0; Promise.resolve(7).then(v => { p = v; }); 'queued'");
    check(contains(r, "\"ok\":true"), "promise job queued");
    r = js_eval(h, "p");
    check(contains(r, "\"result\":\"7\""), "promise job drained before return");

    // The bridge must expose no I/O surface at all.
    r = js_eval(h, "typeof fetch + ',' + typeof setTimeout + ',' + typeof read");
    check(contains(r, "undefined,undefined,undefined"), "no fetch/setTimeout/read builtins");

    // A NUL byte is a legal JS source character (here, inside a string
    // literal); the length-aware bridge must not truncate the script at it.
    r = js_eval(h, std::string("'a\0b'.length", 12));
    check(contains(r, "\"result\":\"3\""), "source with embedded NUL is not truncated");

    // --- ES modules ---------------------------------------------------------
    // Registry-backed loading: the host registers sources, imports resolve
    // against the registry (exact match + ./ and ../ against the referrer).
    r = js_eval_module(h, "bare.js", "globalThis.mod0 = 'bare';");
    check(contains(r, "\"ok\":true"), "module with no imports evaluates");
    r = js_eval(h, "mod0");
    check(contains(r, "\"result\":\"bare\""), "module side effects reach the global");

    r = js_module_register(h, "lib/dep.js", "export const dep = 'dep-ok';");
    check(contains(r, "\"ok\":true"), "module registers");
    r = js_module_register(h, "lib/mid.js",
                           "import { dep } from './dep.js'; export const mid = dep + '+mid';");
    check(contains(r, "\"ok\":true"), "module with relative import registers");
    r = js_eval_module(h, "main.js",
                       "import { mid } from './lib/mid.js'; globalThis.mod1 = mid;");
    check(contains(r, "\"ok\":true"), "static import graph links and evaluates");
    r = js_eval(h, "mod1");
    check(contains(r, "\"result\":\"dep-ok+mid\""), "transitive relative imports resolve");

    r = js_eval_module(h, "missing.js", "import x from './nowhere.js';");
    check(contains(r, "\"ok\":false") && contains(r, "module not registered: nowhere.js"),
          "unregistered import fails with the specifier named");

    // Dynamic import resolves from the same registry; microtasks drain before
    // js_eval_module returns, so the .then has run.
    r = js_eval_module(h, "dyn.js",
                       "globalThis.mod2 = 'pending';"
                       "import('./lib/dep.js').then(ns => { globalThis.mod2 = ns.dep; },"
                       "                             e => { globalThis.mod2 = 'rejected:' + e; });");
    check(contains(r, "\"ok\":true"), "dynamic import evaluates");
    r = js_eval(h, "mod2");
    check(contains(r, "\"result\":\"dep-ok\""), "dynamic import resolved from the registry");

    // Top-level await over an already-resolved promise completes synchronously
    // (the job queue drains before returning).
    r = js_eval_module(h, "tla.js",
                       "const v = await Promise.resolve('tla-ok'); globalThis.mod3 = v;");
    check(contains(r, "\"ok\":true"), "top-level await module settles");
    r = js_eval(h, "mod3");
    check(contains(r, "\"result\":\"tla-ok\""), "top-level await value observed");

    // --- $262 test hooks ----------------------------------------------------
    js_install_test262_hooks(h);
    r = js_eval(h, "typeof $262 + ',' + typeof $262.createRealm + ',' + typeof $262.gc");
    check(contains(r, "object,function,function"), "$262 installed");
    r = js_eval(h, "$262.gc(); $262.evalScript('6 * 7')");
    check(contains(r, "\"result\":\"42\""), "$262.evalScript evaluates in-realm");
    r = js_eval(h, "const ab = new ArrayBuffer(16); $262.detachArrayBuffer(ab); ab.byteLength");
    check(contains(r, "\"result\":\"0\""), "$262.detachArrayBuffer detaches");
    r = js_eval(h, "const realm = $262.createRealm();"
                   "realm.evalScript('var inChild = 123');"
                   "realm.global.inChild + ',' + typeof globalThis.inChild");
    check(contains(r, "\"result\":\"123,undefined\""),
          "$262.createRealm isolates globals but shares objects");
    r = js_eval(h, "typeof $262.IsHTMLDDA + ',' + ($262.IsHTMLDDA == null) + ',' + $262.IsHTMLDDA()");
    check(contains(r, "\"result\":\"undefined,true,null\""),
          "$262.IsHTMLDDA emulates undefined and calls to null");

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
    r = js_eval(h, "while (true) {}");
    std::printf("     %s\n", r.c_str());
    check(contains(r, "\"ok\":false") && contains(r, "interrupted"), "infinite loop interrupted");

    // The runtime must still be usable afterwards.
    r = js_eval(h, "1 + 1");
    check(contains(r, "\"result\":\"2\""), "runtime usable after interrupt");

    // --- interrupt is uncatchable -----------------------------------------
    // The whole security argument: guest JS must not be able to swallow it.
    fire();
    r = js_eval(h, "try { while (true) {} } catch (e) { 'swallowed' } finally { }");
    std::printf("     %s\n", r.c_str());
    check(contains(r, "\"ok\":false") && !contains(r, "swallowed"),
          "interrupt is uncatchable by try/catch");

    r = js_eval(h, "2 * 3");
    check(contains(r, "\"result\":\"6\""), "runtime usable after uncatchable interrupt");

    // Regression: an interrupt that fires consumes SpiderMonkey's armed state
    // (handleInterrupt clears interruptBits_ before calling us). If nothing
    // re-arms, the SECOND interrupt on the same runtime never fires and this
    // eval hangs forever. That is only visible from the third interrupt onward
    // in fallback mode, so exercise it repeatedly.
    for (int i = 0; i < 3; i++) {
        fire();
        r = js_eval(h, "while (true) {}");
        check(contains(r, "\"ok\":false") && contains(r, "interrupted"),
              "repeated interrupt still fires (re-armed)");
    }

    // --- an interrupt that was never requested must not fire ---------------
    r = js_eval(h, "let n = 0; for (let i = 0; i < 3000000; i++) n += i; n > 0");
    check(contains(r, "\"ok\":true") && contains(r, "\"result\":\"true\""),
          "long loop completes when no interrupt is pending");

    // --- recursion is bounded, not a wasm trap -----------------------------
    r = js_eval(h, "function f(){ return f(); } try { f() } catch (e) { 'caught:' + e.name }");
    std::printf("     %s\n", r.c_str());
    check(contains(r, "caught:InternalError") || contains(r, "too much recursion"),
          "native stack quota turns runaway recursion into a JS error");

    // --- heap cap ----------------------------------------------------------
    // Report what was actually thrown: an assertion that merely checks the loop
    // stopped would also pass if some unrelated error ended it.
    r = js_eval(h, "let a = [];\n"
                   "try { for(;;) a.push(new Array(100000).fill(0)); a = null; 'never' }\n"
                   "catch (e) { a = null; 'threw:' + String(e) }");
    std::printf("     %s\n", r.c_str());
    check(contains(r, "out of memory") || contains(r, "allocation size overflow") ||
              (contains(r, "\"ok\":false") && contains(r, "memory")),
          "heap cap stops runaway allocation with an out-of-memory error");

    js_close(h);
    check(true, "js_close");

    std::printf("\n%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
