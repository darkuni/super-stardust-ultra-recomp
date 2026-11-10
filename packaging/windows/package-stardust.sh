#!/usr/bin/env bash
# Builds dist/stardust-win64.zip: one folder with stardust.exe (the loader, which unpacks and
# prepares the game itself on the first start), the MSYS2 CLANG64 DLLs it loads, and a README.
# No launcher, no Python. Run from an MSYS2 CLANG64 shell after `bash build.sh`.
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
source ./msys2-env.sh
[[ -f out/gow3-probe.exe ]] || { echo 'Build first: bash build.sh' >&2; exit 1; }

dest=out/stage/stardust
rm -rf -- out/stage
mkdir -p "$dest"
llvm-strip --strip-debug -o "$dest/stardust.exe" out/gow3-probe.exe
# Every DLL it loads from the CLANG64 tree (SDL3, FFmpeg, zlib, Vulkan loader, ...).
ldd "$dest/stardust.exe" | awk '/\/clang64\/bin\// {print $3}' | sort -u | while read -r dll; do
    cp -u "$dll" "$dest/"
done
cp packaging/README-stardust.txt "$dest/README.txt"
cp LICENSE "$dest/LICENSE.txt"
# No game files and nothing a player made may ship.
forbidden=$(find "$dest" \( -iname eboot.bin -o -iname sce_module -o -iname sce_sys -o -iname user -o -iname game \
    -o -iname out -o -iname '*.log' -o -iname '*.ini' -o -iname '*.pkg' -o -iname '*.prx' -o -iname '*.spv' \) -print)
[[ -z $forbidden ]] || { echo "Package holds files that must not ship: $forbidden" >&2; exit 1; }
mkdir -p dist
rm -f dist/stardust-win64.zip
(cd out/stage && powershell -NoProfile -Command "Compress-Archive -Path stardust -DestinationPath ../../dist/stardust-win64.zip")
du -sh "$dest" dist/stardust-win64.zip
