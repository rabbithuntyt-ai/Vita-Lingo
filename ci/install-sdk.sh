#!/bin/bash
# Installs the prebuilt VitaSDK plus the libraries mGBA's Vita port links against.
set -euxo pipefail
sudo apt-get update -q
sudo apt-get install -y -q cmake make git curl xz-utils bzip2 python3 pkg-config libarchive-tools fakeroot
sudo mkdir -p "$VITASDK" && sudo chown -R "$USER" "$VITASDK"
export PATH="$VITASDK/bin:$PATH"
git clone --depth 1 https://github.com/vitasdk/vdpm /tmp/vdpm
cd /tmp/vdpm
ls
./bootstrap-vitasdk.sh
./install-all.sh || true
ls "$VITASDK/bin" | head -50
ls "$VITASDK/arm-vita-eabi/lib" | grep -E "vita2d|png|libz" || true
