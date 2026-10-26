#!/bin/bash
# Installs the prebuilt VitaSDK plus the libraries mGBA's Vita port links against.
set -euxo pipefail
sudo apt-get update -q
sudo apt-get install -y -q cmake make git curl xz-utils bzip2 python3 pkg-config
git clone --depth 1 https://github.com/vitasdk/vdpm /tmp/vdpm
cd /tmp/vdpm
./bootstrap-vitasdk.sh   # creates $VITASDK itself
export PATH="$VITASDK/bin:$PATH"
vdpm search vita2d || true
vdpm search png || true
vdpm search zlib || true
pick() { # first package name that exists
	for n in "$@"; do if vdpm info "$n" >/dev/null 2>&1; then echo "$n"; return; fi; done
	echo "none of: $*" >&2; return 1
}
PKGS="$(pick libvita2d vita2d) $(pick libpng png) $(pick zlib libz) $(pick openssl) $(pick curl)"
vdpm install --noconfirm $PKGS < /dev/null || vdpm install $PKGS < <(yes)
ls "$VITASDK/arm-vita-eabi/lib" | grep -Ei "vita2d|png|libz|curl|ssl|crypto" || true
ls "$VITASDK/arm-vita-eabi/include" | grep -Ei "vita2d|png|zlib" || true
