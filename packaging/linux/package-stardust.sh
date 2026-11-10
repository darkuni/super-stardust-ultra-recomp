#!/usr/bin/env bash
# Builds dist/stardust-linux-x86_64.tar.gz: one folder with `stardust` (the loader, which unpacks
# and prepares the game itself on the first start), lib/ with the GPU library and the shared
# libraries a SteamOS / desktop system may lack, and a README. No launcher, no Python.
# The system's graphics, sound and C libraries are used. FFmpeg's video API loaders (libva,
# libvdpau, libvpl, OpenCL) come along: SteamOS may lack them, and the decoding is in software.
# Run after `bash build.sh` on Ubuntu 24.04 (glibc 2.39, as SteamOS 3.7+).
set -euo pipefail
cd -- "$(dirname -- "$0")/../.."
[[ -f out/gow3-probe && -f out/gpu/libgow3gpu.so ]] || { echo 'Build first: bash build.sh' >&2; exit 1; }

dest=out/stage/stardust
rm -rf -- out/stage
mkdir -p "$dest/lib"
install -m755 out/gow3-probe "$dest/stardust"
install -m755 out/gpu/libgow3gpu.so "$dest/lib/"
excluded='^(ld-linux.*|libc|libm|libdl|librt|libpthread|libresolv|libutil|libnsl|libanl|libvulkan|libGL|libGLX|libGLdispatch|libEGL|libOpenGL|libgbm|libdrm.*|libX11|libX11-xcb|libxcb.*|libXau|libXdmcp|libXext|libXfixes|libXrender|libXi|libXcursor|libXrandr|libwayland-.*|libasound|libudev|libpulse.*|libfontconfig|libfreetype|libnvidia-.*|libcuda|libz|libstdc\+\+|libgcc_s)\.so'
declare -A seen
bundle() {
    local elf=$1 line lib name
    while read -r line; do
        lib=$(awk '/=> \//{print $3}' <<<"$line")
        [[ -n $lib ]] || continue
        name=$(basename -- "$lib")
        [[ ${seen[$name]:-} ]] && continue
        seen[$name]=1
        [[ $name =~ $excluded ]] && continue
        install -m755 "$lib" "$dest/lib/$name"
        bundle "$lib"
    done < <(ldd "$elf")
}
bundle out/gpu/libgow3gpu.so
bundle out/gow3-probe
# GCC 14's C++ runtime: newer than some systems' (the GPU library needs it).
for name in libstdc++.so.6 libgcc_s.so.1; do
    install -m755 "$(ldd out/gpu/libgow3gpu.so | awk -v n="$name" '$1==n{print $3}')" "$dest/lib/$name"
done
strip --strip-unneeded "$dest/stardust" "$dest"/lib/*.so* 2>/dev/null || true
# Everything is found next to the program, wherever the folder is.
patchelf --set-rpath '$ORIGIN/lib' "$dest/stardust"
for so in "$dest"/lib/*.so*; do patchelf --set-rpath '$ORIGIN' "$so"; done
install -m644 packaging/README-stardust.txt "$dest/README.txt"
install -m644 LICENSE "$dest/LICENSE.txt"
missing=$(ldd "$dest/stardust" | grep 'not found' || true)
[[ -z $missing ]] || { echo "Unresolved libraries: $missing" >&2; exit 1; }
max_glibc=${GOW3_MAX_GLIBC:-2.39}
need=$(find "$dest" -type f \( -name '*.so*' -o -name stardust \) -exec objdump -T {} + 2>/dev/null |
       grep -o 'GLIBC_2\.[0-9.]*' | sed 's/GLIBC_//' | sort -uV | tail -1 || true)
if [[ -n $need && $(printf '%s\n%s\n' "$need" "$max_glibc" | sort -V | tail -1) != "$max_glibc" ]]; then
    echo "The package needs glibc $need, newer than $max_glibc" >&2
    exit 1
fi
mkdir -p dist
tar -C out/stage -czf dist/stardust-linux-x86_64.tar.gz stardust
du -sh "$dest" dist/stardust-linux-x86_64.tar.gz
