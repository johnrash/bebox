#!/bin/sh
# Extracts the BeBox boot ROM update images (beos/etc/bebox_bootmain.image)
# from BeOS PowerPC install CDs into work/roms/, for booting the BFS based
# releases (AA/DR9 and later) without running ROMUpdater first.
#
# Uses bebox-img's own BFS reader (no mounting, no root):
#
#   tools/extract-rom-images.sh
#
# Images: pr2_bebox_bootmain.image (PR2; also boots AA), r3_..., r45_...
# The app does the same from Preferences > ROM & NVRAM > Boot ROM image.

cd "$(dirname "$0")/.." || exit 1
make -s -C tests/linux FAST=1 out/fast/bebox-img || exit 1
BEBOX_IMG=tests/linux/out/fast/bebox-img
mkdir -p work/roms

extract() {
	image=$1 name=$2
	if [ ! -f "$image" ]; then
		echo "skip $name: no $image"
		return
	fi
	"$BEBOX_IMG" extract "$image" /beos/etc/bebox_bootmain.image \
		"work/roms/${name}_bebox_bootmain.image" >/dev/null \
		&& echo "work/roms/${name}_bebox_bootmain.image"
}

extract "work/media/PR2/BeOS Preview Release 2 (Be, Inc.)(1997).iso" pr2
extract "work/media/R3/BeOS Release 3 PowerPC.iso" r3
extract "work/media/R4.5/BeOS-R4.5/BeOS-R4.5.cue" r45
