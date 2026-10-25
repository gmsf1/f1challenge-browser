#!/bin/sh
# Copy the page and the wasm build into cloud/site/public (what `cf deploy` uploads). Run build_web.sh first.
set -e
cd "$(dirname "$0")/.."
out=cloud/site/public
rm -rf "$out" && mkdir -p "$out/build"
cp web/index.html web/loader.js web/netplay.js web/audio-worklet.js "$out/"
cp build_web/www/recomp.js build_web/www/recomp.wasm "$out/build/"
# SharedArrayBuffer (wasm threads) needs a cross-origin isolated page
printf '/*\n  Cross-Origin-Opener-Policy: same-origin\n  Cross-Origin-Embedder-Policy: require-corp\n  Cross-Origin-Resource-Policy: same-origin\n' > "$out/_headers"
