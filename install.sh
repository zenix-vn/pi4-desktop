#!/bin/sh
# Cai driver panel + cam ung JD9366 len Raspberry Pi 4 (Raspberry Pi OS Bookworm tro len).
# Chay: sudo ./install.sh   (lai sau moi lan nang cap kernel)
set -e
[ "$(id -u)" = 0 ] || { echo "Chay bang sudo"; exit 1; }
cd "$(dirname "$0")"

KVER=$(uname -r)
if [ ! -d "/lib/modules/$KVER/build" ]; then
	apt-get update
	apt-get install -y build-essential device-tree-compiler linux-headers-rpi-v8 || \
	apt-get install -y build-essential device-tree-compiler "linux-headers-$KVER"
fi
command -v dtc >/dev/null || apt-get install -y device-tree-compiler

make clean >/dev/null 2>&1 || true
make

DEST=/lib/modules/$KVER/extra
mkdir -p "$DEST"
cp panel-jd9366.ko jd9366-touch.ko "$DEST/"
depmod -a "$KVER"

BOOT=/boot/firmware
[ -d "$BOOT/overlays" ] || BOOT=/boot
cp jd9366-pi4.dtbo "$BOOT/overlays/"

CFG=$BOOT/config.txt
if ! grep -q '^dtoverlay=jd9366-pi4' "$CFG"; then
	cp "$CFG" "$CFG.bak-jd9366"
	# Tat tu nhan man DSI chinh hang, de overlay nay quan ly DSI1
	sed -i 's/^display_auto_detect=1/display_auto_detect=0/' "$CFG"
	{
		echo
		echo '[all]'
		echo '# Man OSPTEK 10.1" JD9366 (Pi4-Display)'
		grep -q '^display_auto_detect=0' "$CFG" || echo 'display_auto_detect=0'
		grep -q '^dtoverlay=vc4-kms-v3d' "$CFG" || echo 'dtoverlay=vc4-kms-v3d'
		echo 'ignore_lcd=1'
		echo 'dtoverlay=jd9366-pi4'
	} >> "$CFG"
	echo "Da sua $CFG (ban cu: $CFG.bak-jd9366)"
fi

echo "Xong. Khoi dong lai: sudo reboot"
