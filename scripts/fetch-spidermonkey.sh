#!/usr/bin/env bash
#
# fetch-spidermonkey.sh — materialise the SpiderMonkey wasm32-wasi static
# library and its public headers under deps/spidermonkey/.
#
# This is spidermonkey-wasm's `build_commands.configure`. Unlike python-wasm /
# perl-wasm, there is no upstream source tree for wasmify to configure, compile
# and replay: SpiderMonkey's wasi build is published as a prebuilt archive by
# the StarlingMonkey project, which is what the `starlingmonkey` submodule is
# here for. The submodule pins the engine version (SM_TAG below); this script
# reads that pin and downloads the matching artifact. wasmify then links the
# archive into our wasm via wasmify.json's wasm_build.prebuilt_archives.
#
# Why not build SpiderMonkey from mozilla-central? It takes hours, needs a Rust
# and Python toolchain, and yields the same symbols. The only thing a source
# build adds is SpiderMonkey's INTERNAL headers (vm/JSContext.h et al), which
# the dist tarball omits — and js.cc deliberately depends on the public JSAPI
# only. See js.h's "Interruption support" section.
#
# Outputs (git-ignored):
#   deps/spidermonkey/libspidermonkey.a   ThinLTO bitcode archive, wasm32-wasi
#   deps/spidermonkey/include/            public JSAPI headers + js-confdefs.h
#   deps/spidermonkey/.tag                the SM_TAG the tree was fetched for
set -euo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$repo_root"

sm_cmake=starlingmonkey/cmake/spidermonkey.cmake
if [[ ! -f $sm_cmake ]]; then
    echo "error: $sm_cmake not found. Run: git submodule update --init starlingmonkey" >&2
    exit 1
fi

# The single line `set(SM_TAG FIREFOX_..._RELEASE_STARLING)` is the engine pin.
# Parsing it (rather than duplicating the tag here) means `git submodule update`
# is the only place a SpiderMonkey version bump has to happen.
SM_TAG=$(sed -n 's/^set(SM_TAG \([A-Za-z0-9_]*\))$/\1/p' "$sm_cmake")
if [[ -z ${SM_TAG:-} ]]; then
    echo "error: could not parse SM_TAG from $sm_cmake" >&2
    exit 1
fi

# `release` matches StarlingMonkey's default (non-Debug) build type. A `debug`
# archive exists too, but it is compiled with -DDEBUG, and js-confdefs.h then
# hard-errors unless every translation unit including it also defines DEBUG.
BUILD_TYPE=release
url="https://github.com/bytecodealliance/starlingmonkey/releases/download/libspidermonkey_${SM_TAG}/spidermonkey-static-${BUILD_TYPE}.tar.gz"

dest=deps/spidermonkey
stamp=$dest/.tag

up_to_date=false
if [[ -f $stamp && $(cat "$stamp") == "$SM_TAG" && -f $dest/libspidermonkey.a ]]; then
    up_to_date=true
    # A local (with-Intl) archive is a different flavor than the StarlingMonkey
    # prebuilt at the same tag; jsrust presence is the flavor marker.
    if [[ -n ${SPIDERMONKEY_LOCAL_ARCHIVE:-} && ! -f $dest/libjsrust.a ]]; then
        up_to_date=false
    fi
fi
if $up_to_date; then
    echo "[fetch-spidermonkey] up to date: $SM_TAG"
    exit 0
fi

echo "[fetch-spidermonkey] SM_TAG=$SM_TAG"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# SPIDERMONKEY_LOCAL_ARCHIVE points at a locally built engine tarball — the
# with-Intl archive scripts/build-engine-intl.sh produces (CI hands it across
# jobs as an artifact). When set, it replaces the StarlingMonkey download.
if [[ -n ${SPIDERMONKEY_LOCAL_ARCHIVE:-} ]]; then
    echo "[fetch-spidermonkey] using local archive $SPIDERMONKEY_LOCAL_ARCHIVE"
    cp "$SPIDERMONKEY_LOCAL_ARCHIVE" "$tmp/sm.tar.gz"
else
    echo "[fetch-spidermonkey] downloading $url"
    curl --fail --location --progress-bar "$url" -o "$tmp/sm.tar.gz"
fi

# The tarball's single top-level directory is spidermonkey-dist-<type>/, holding
# libspidermonkey.a and include/. Strip it so paths are stable regardless of the
# build type baked into the artifact name.
mkdir -p "$tmp/x"
tar -xzf "$tmp/sm.tar.gz" -C "$tmp/x" --strip-components=1

required_files=(libspidermonkey.a include/jsapi.h include/js-confdefs.h)
if [[ -n ${SPIDERMONKEY_LOCAL_ARCHIVE:-} ]]; then
    # The with-Intl flavor also ships jsrust (encoding_rs + ICU4X + Temporal),
    # which wasmify.json's prebuilt_archives links instead of rust/'s staticlib.
    required_files+=(libjsrust.a)
fi
for required in "${required_files[@]}"; do
    if [[ ! -e $tmp/x/$required ]]; then
        echo "error: $required missing from the engine archive" >&2
        exit 1
    fi
done

rm -rf "$dest"
mkdir -p "$(dirname "$dest")"
mv "$tmp/x" "$dest"
echo "$SM_TAG" >"$stamp"

echo "[fetch-spidermonkey] installed $dest ($(du -sh "$dest" | cut -f1))"
