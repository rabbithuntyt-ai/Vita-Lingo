#!/bin/bash
# Installs the prebuilt VitaSDK plus the libraries mGBA's Vita port links against.
set -euxo pipefail
sudo apt-get update -q
sudo apt-get install -y -q cmake make git curl xz-utils bzip2 python3 pkg-config libarchive-tools fakeroot
git clone --depth 1 https://github.com/vitasdk/vdpm /tmp/vdpm
cd /tmp/vdpm
./bootstrap-vitasdk.sh   # must create $VITASDK itself
export PATH="$VITASDK/bin:$PATH"
vdpm install vita2d libpng zlib || ./install-all.sh
ls "$VITASDK/bin" | head -60
ls "$VITASDK/arm-vita-eabi/lib" | grep -Ei "vita2d|png|libz" || true
