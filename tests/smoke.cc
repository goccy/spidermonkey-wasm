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
    std::string r = js_eval(h, "1 + 2", sizeof("1 + 2") - 1);
    std::printf("     %s\n", r.c_str());
    check(contains(r, "\"ok\":true") && contains(r, "\"result\":\"3\""), "eval arithmetic");

    r = js_eval(h, "globalThis.x = 41; x + 1", sizeof("globalThis.x = 41; x + 1") - 1);
    check(contains(r, "\"result\":\"42\""), "eval global persists across calls");

    r = js_eval(h, "print('hello'); console.error('bad'); 'done'", sizeof("print('hello'); console.error('bad'); 'done'") - 1);
    std::printf("     %s\n", r.c_str());
    check(contains(r, "\"stdout\":\"hello\\n\"") && contains(r, "\"stderr\":\"bad\\n\""),
          "print/console captured");

    r = js_eval(h, "throw new Error('boom')", sizeof("throw new Error('boom')") - 1);
    check(contains(r, "\"ok\":false") && contains(r, "boom"), "uncaught exception reported");

    r = js_eval(h, "JSON.stringify({a:[1,2]})", sizeof("JSON.stringify({a:[1,2]})") - 1);
    check(contains(r, "{\\\"a\\\":[1,2]}"), "standard classes available");

    r = js_eval(h, "let p = 0; Promise.resolve(7).then(v => { p = v; }); 'queued'", sizeof("let p = 0; Promise.resolve(7).then(v => { p = v; }); 'queued'") - 1);
    check(contains(r, "\"ok\":true"), "promise job queued");
    r = js_eval(h, "p", sizeof("p") - 1);
    check(contains(r, "\"result\":\"7\""), "promise job drained before return");

    // The bridge must expose no I/O surface at all.
    r = js_eval(h, "typeof fetch + ',' + typeof setTimeout + ',' + typeof read", sizeof("typeof fetch + ',' + typeof setTimeout + ',' + typeof read") - 1);
    check(contains(r, "undefined,undefined,undefined"), "no fetch/setTimeout/read builtins");

    // A NUL byte is a legal JS source character (here, inside a string
    // literal); the length-aware bridge must not truncate the script at it.
    r = js_eval(h, "'a\0b'.length", 12);
    check(contains(r, "\"result\":\"3\""), "source with embedded NUL is not truncated");

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
    check(contains(r, "\"result\":\"2\""), "runtime usable after interrupt");

    // --- interrupt is uncatchable -----------------------------------------
    // The whole security argument: guest JS must not be able to swallow it.
    fire();
    r = js_eval(h, "try { while (true) {} } catch (e) { 'swallowed' } finally { }", sizeof("try { while (true) {} } catch (e) { 'swallowed' } finally { }") - 1);
    std::printf("     %s\n", r.c_str());
    check(contains(r, "\"ok\":false") && !contains(r, "swallowed"),
          "interrupt is uncatchable by try/catch");

    r = js_eval(h, "2 * 3", sizeof("2 * 3") - 1);
    check(contains(r, "\"result\":\"6\""), "runtime usable after uncatchable interrupt");

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
    check(contains(r, "\"ok\":true") && contains(r, "\"result\":\"true\""),
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

    std::printf("\n%s (%d failures)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
