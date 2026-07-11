// Re-export the crates so their `#[no_mangle]` C-ABI entry points survive into
// the staticlib. Without a `pub use`, a crate that nothing in this file names is
// dropped as an unused dependency and its symbols never reach the archive —
// SpiderMonkey's undefined `encoding_mem_*` / `install_rust_hooks` would then
// stay undefined at the wasm link.
//
// Nothing in Go or C++ calls these directly; libspidermonkey.a does.
pub use rust_encoding;
pub use rust_hooks;
