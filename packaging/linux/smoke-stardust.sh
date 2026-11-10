#!/usr/bin/env bash
# Starts the packaged stardust in a clean container: installs only what a desktop or SteamOS
# always has (Vulkan loader, X11 client libraries, fonts, zlib), unpacks the archive and runs
# `stardust --cpu-features`, which loads every library and exits without a game.
# Usage (CI): docker run --rm -v "$PWD:/w:ro" <image> bash /w/packaging/linux/smoke-stardust.sh /w/dist/stardust-linux-x86_64.tar.gz
set -euo pipefail
archive=$1
if command -v apt-get >/dev/null; then
    apt-get update -qq
    DEBIAN_FRONTEND=noninteractive apt-get install -y -qq libvulkan1 libx11-6 libx11-xcb1 libxext6 libxfixes3 \
        libxrender1 libxcb1 libxcb-dri3-0 libxcb-render0 libxcb-shm0 libfontconfig1 libfreetype6 libdrm2 zlib1g >/dev/null
elif command -v dnf >/dev/null; then
    dnf install -y -q vulkan-loader libX11 libX11-xcb libXext libXfixes libXrender libxcb fontconfig freetype libdrm zlib >/dev/null
elif command -v pacman >/dev/null; then
    pacman -Sy --noconfirm --needed vulkan-icd-loader libx11 libxext libxfixes libxrender libxcb fontconfig freetype2 \
        libdrm zlib >/dev/null
fi
grep -m1 PRETTY_NAME /etc/os-release
ldd --version | sed -n 1p
work=$(mktemp -d)
tar -C "$work" -xzf "$archive"
missing=$(ldd "$work/stardust/stardust" | grep 'not found' || true)
if [[ -n $missing ]]; then
    echo "FAIL: libraries not found:" >&2
    echo "$missing" >&2
    exit 1
fi
"$work/stardust/stardust" --cpu-features
echo "OK: stardust starts"
