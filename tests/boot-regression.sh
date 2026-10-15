#!/bin/sh
# BeOS boot regression: boots release media headless and checks for kernel
# debug output markers on serial4 (F1 held at power on). The installed
# disks are used through throwaway QCOW2 overlays, so they stay unchanged.
# Screenshots of the final frames go to $OUT for a look.
#
#   tests/boot-regression.sh [case...]     (default: all cases)
#   THREADS=1 tests/boot-regression.sh     the same, each emulated CPU on a
#                                          host thread of its own
#
# Media (override with environment variables):
#   DR6_CD   work/media/DR6/BeOS_1_1d6.iso
#   DR7_CD   work/media/DR7/BeOS_1_1d7_1.iso
#   DR8_CD   work/media/DR8iso/BeOS_Developer_Release_8.iso
#   DR8_DISK work/disks/dr8.qcow2 (DR8 installed by its Installer)
#   DR6_DISK, DR6_NVRAM  work/disks/dr6.qcow2, dr6.nvram (DR6 installed
#            by its Installer, made the startup disk)
#   AA_CD, PR1_CD, PR2_CD, R3_CD, R45_CD (BFS releases; they need the boot
#   ROM images from tools/extract-rom-images.sh in work/roms; AA and PR1
#   have none of their own and boot with PR2's)
# Not covered: DR8.2 (a Mac HFS update CD).
#   R3_DISK, R45_DISK  work/disks/r3.qcow2, r45.qcow2 (installed by the
#            release's Installer), with R3_NVRAM, R45_NVRAM
#            work/disks/r3.nvram, r45.nvram (boot volume set by it)
#   R45NET_DISK, R45NET_NVRAM  work/disks/r45net.qcow2, r45net.nvram (R4.5
#            with the NE2000 configured for DHCP in its Network preferences)
#   HOBBIT_DIR  Hobbit (the Hobbit prototype's disk images: "Hard Drive
#            Image/hobbit-930800009.img" ("pixels", November 1994),
#            hobbit-9308000058.img ("be", December 1994),
#            Hobbit-9308000044.img ("be3", March 1994) and "Floppy
#            Images/installer.img")

cd "$(dirname "$0")/.." || exit 1
BIN=tests/linux/out/fast
OUT=${OUT:-tests/linux/out/boot}
DR6_CD=${DR6_CD:-work/media/DR6/BeOS_1_1d6.iso}
DR7_CD=${DR7_CD:-work/media/DR7/BeOS_1_1d7_1.iso}
DR8_CD=${DR8_CD:-work/media/DR8iso/BeOS_Developer_Release_8.iso}
DR8_DISK=${DR8_DISK:-work/disks/dr8.qcow2}
DR6_DISK=${DR6_DISK:-work/disks/dr6.qcow2}
DR6_NVRAM=${DR6_NVRAM:-work/disks/dr6.nvram}
AA_CD=${AA_CD:-work/media/AA/BeOS Advanced Access.iso}
PR1_CD=${PR1_CD:-work/media/PR1/BeOS Preview Release (230105) (Be, Inc.) (1997).iso}
PR2_CD=${PR2_CD:-work/media/PR2/BeOS Preview Release 2 (Be, Inc.)(1997).iso}
R3_CD=${R3_CD:-work/media/R3/BeOS Release 3 PowerPC.iso}
R45_CD=${R45_CD:-work/media/R4.5/BeOS-R4.5/BeOS-R4.5.cue}
R3_DISK=${R3_DISK:-work/disks/r3.qcow2}
R3_NVRAM=${R3_NVRAM:-work/disks/r3.nvram}
R45_DISK=${R45_DISK:-work/disks/r45.qcow2}
R45_NVRAM=${R45_NVRAM:-work/disks/r45.nvram}
R45NET_DISK=${R45NET_DISK:-work/disks/r45net.qcow2}
R45NET_NVRAM=${R45NET_NVRAM:-work/disks/r45net.nvram}
HOBBIT_DIR=${HOBBIT_DIR:-Hobbit}

make -C tests/linux FAST=1 out/fast/bebox-headless out/fast/bebox-img \
	>/dev/null \
	|| exit 1
mkdir -p "$OUT"

failures=0
run() {
	name=$1; seconds=$2; shift 2
	markers=$1; shift
	log="$OUT/$name.log"
	# F1 held at power on: kernel debug output on serial 4
	"$BIN/bebox-headless" ${THREADS:+--threads} --rom-dir ROMs \
		--seconds "$seconds" \
		${F1:---key 0:F1:5500} --screenshot "$OUT/$name.png" "$@" \
		>"$log" 2>&1
	missing=""
	old_ifs=$IFS; IFS='|'
	for marker in $markers; do
		grep -q "$marker" "$log" || missing="$missing '$marker'"
	done
	IFS=$old_ifs
	if [ -n "$missing" ]; then
		echo "FAIL $name: missing$missing (see $log)"
		failures=$((failures + 1))
	else
		echo "ok   $name ($(grep -o 'in [0-9.]* s ([0-9]*%)' "$log"))"
	fi
}

overlay() {
	rm -f "$2"
	$BIN/bebox-img create -b "$(realpath "$1")" "$2" >/dev/null
}

want() {
	[ $# -eq 0 ] && return 0
	for c in $CASES; do [ "$c" = "$1" ] && return 0; done
	return 1
}
CASES="$*"

# DR6 boots on the 1.1d7 boot ROM with its PCI table given as the older
# "pci_table_v2" (src/core/board/BootItems.h)
if [ -z "$CASES" ] || want dr6-cd; then
	if [ -f "$DR6_CD" ]; then
		run dr6-cd 40 "pci_table_v2|TOSHIBA CD-ROM|HELLO WORLD|Starting system/Browser" \
			--scsi3-cd "$DR6_CD" --log machine=info
	else
		echo "skip dr6-cd (no $DR6_CD)"
	fi
fi
if [ -z "$CASES" ] || want dr6-disk; then
	if [ -f "$DR6_DISK" ] && [ -f "$DR6_NVRAM" ]; then
		overlay "$DR6_DISK" "$OUT/dr6-disk.qcow2"
		cp "$DR6_NVRAM" "$OUT/dr6-disk.nvram"
		run dr6-disk 40 "Booting from device: /dev/scsi_disk_000|HELLO WORLD|Starting system/Browser" \
			--scsi0-disk "$OUT/dr6-disk.qcow2" --nvram "$OUT/dr6-disk.nvram"
	else
		echo "skip dr6-disk (no $DR6_DISK or $DR6_NVRAM)"
	fi
fi
if [ -z "$CASES" ] || want dr7-cd; then
	if [ -f "$DR7_CD" ]; then
		run dr7-cd 40 "Booting from device|HELLO WORLD|Starting bin/sh" \
			--scsi3-cd "$DR7_CD"
	else
		echo "skip dr7-cd (no $DR7_CD)"
	fi
fi
if [ -z "$CASES" ] || want dr8-cd; then
	if [ -f "$DR8_CD" ]; then
		run dr8-cd 40 "Booting from device|HELLO WORLD|Starting bin/sh" \
			--scsi3-cd "$DR8_CD"
	else
		echo "skip dr8-cd (no $DR8_CD)"
	fi
fi
for model in 66 133; do
	if [ -z "$CASES" ] || want dr8-disk-$model; then
		if [ -f "$DR8_DISK" ]; then
			overlay "$DR8_DISK" "$OUT/dr8-$model.qcow2"
			run dr8-disk-$model 40 \
				"Booting from device: /dev/scsi_disk_000|HELLO WORLD|swap_size" \
				--model $model --scsi0-disk "$OUT/dr8-$model.qcow2"
			$BIN/bebox-img check "$OUT/dr8-$model.qcow2" >/dev/null \
				|| { echo "FAIL dr8-disk-$model: overlay check"; \
					failures=$((failures + 1)); }
		else
			echo "skip dr8-disk-$model (no $DR8_DISK)"
		fi
	fi
done

# BFS releases: the newer boot ROMs stop at their boot menu (no boot volume
# configured yet); Return boots the CD. The kernel's keyboard and mouse
# driver enabling the mouse (controller command d4) marks a booted kernel.
bfs_case() {
	name=$1 cd=$2 rom=$3 seconds=$4 enter=${5:-30000}
	if [ -z "$CASES" ] || want "$name"; then
		if [ -f "$cd" ] && [ -f "$rom" ]; then
			# without debug output (the R3 ROM gets slow printing it)
			F1=" " run "$name" "$seconds" "write d4 to command" \
				--main-rom "$rom" \
				--scsi3-cd "$cd" --key "$enter":Enter --log kbd=debug
		else
			echo "skip $name (no $cd or $rom)"
		fi
	fi
}
bfs_case aa-cd "$AA_CD" work/roms/pr2_bebox_bootmain.image 100
bfs_case pr1-cd "$PR1_CD" work/roms/pr2_bebox_bootmain.image 100
bfs_case pr2-cd "$PR2_CD" work/roms/pr2_bebox_bootmain.image 100
bfs_case r3-cd "$R3_CD" work/roms/r3_bebox_bootmain.image 100 30000
bfs_case r45-cd "$R45_CD" work/roms/r45_bebox_bootmain.image 100

# installed BFS disks (no CD attached), both models
disk_case() {
	release=$1 disk=$2 nvram=$3 rom=$4
	for model in 66 133; do
		name=$release-disk-$model
		if [ -n "$CASES" ] && ! want $name; then
			continue
		fi
		if [ ! -f "$disk" ] || [ ! -f "$nvram" ] || [ ! -f "$rom" ]; then
			echo "skip $name (no $disk, $nvram or $rom)"
			continue
		fi
		overlay "$disk" "$OUT/$name.qcow2"
		cp "$nvram" "$OUT/$name.nvram"
		F1=" " run $name 60 "write d4 to command" \
			--model $model --main-rom "$rom" --nvram "$OUT/$name.nvram" \
			--scsi0-disk "$OUT/$name.qcow2" --log kbd=debug
		$BIN/bebox-img check "$OUT/$name.qcow2" >/dev/null \
			|| { echo "FAIL $name: overlay check"; \
				failures=$((failures + 1)); }
	done
}
disk_case r3 "$R3_DISK" "$R3_NVRAM" work/roms/r3_bebox_bootmain.image
disk_case r45 "$R45_DISK" "$R45_NVRAM" work/roms/r45_bebox_bootmain.image

# the Cirrus Logic card (the others use the default #9 GXE64)
if [ -z "$CASES" ] || want dr8-disk-cirrus; then
	if [ -f "$DR8_DISK" ]; then
		overlay "$DR8_DISK" "$OUT/dr8-cirrus.qcow2"
		run dr8-disk-cirrus 40 \
			"Booting from device: /dev/scsi_disk_000|HELLO WORLD|swap_size" \
			--video cirrus --scsi0-disk "$OUT/dr8-cirrus.qcow2"
	else
		echo "skip dr8-disk-cirrus (no $DR8_DISK)"
	fi
fi
if [ -z "$CASES" ] || want r45-disk-cirrus; then
	if [ -f "$R45_DISK" ] && [ -f "$R45_NVRAM" ]; then
		overlay "$R45_DISK" "$OUT/r45-cirrus.qcow2"
		cp "$R45_NVRAM" "$OUT/r45-cirrus.nvram"
		F1=" " run r45-disk-cirrus 60 "write d4 to command" --video cirrus \
			--main-rom work/roms/r45_bebox_bootmain.image \
			--nvram "$OUT/r45-cirrus.nvram" \
			--scsi0-disk "$OUT/r45-cirrus.qcow2" --log kbd=debug
	else
		echo "skip r45-disk-cirrus (no $R45_DISK or $R45_NVRAM)"
	fi
fi

# Ethernet: R4.5 configures its NE2000 by DHCP through user mode
# networking (the card transmits the DHCP requests and gets answers)
if [ -z "$CASES" ] || want r45-net; then
	if [ -f "$R45NET_DISK" ] && [ -f "$R45NET_NVRAM" ]; then
		overlay "$R45NET_DISK" "$OUT/r45-net.qcow2"
		cp "$R45NET_NVRAM" "$OUT/r45-net.nvram"
		F1=" " run r45-net 70 "ne2000: sent" --net \
			--main-rom work/roms/r45_bebox_bootmain.image \
			--nvram "$OUT/r45-net.nvram" \
			--scsi0-disk "$OUT/r45-net.qcow2" --log isa=debug
	else
		echo "skip r45-net (no $R45NET_DISK or $R45NET_NVRAM)"
	fi
fi

# Shared folder: R4.5 mounts a host folder as a BFS disk; the volume is
# synced back (nothing changed) and removed at the end
if [ -z "$CASES" ] || want r45-share; then
	if [ -f "$R45_DISK" ] && [ -f "$R45_NVRAM" ]; then
		share="$OUT/r45-share-folder"
		rm -rf "$share"
		mkdir -p "$share/docs"
		echo "shared with BeOS" > "$share/docs/readme.txt"
		overlay "$R45_DISK" "$OUT/r45-share.qcow2"
		cp "$R45_NVRAM" "$OUT/r45-share.nvram"
		TMPDIR="$OUT" F1=" " run r45-share 60 "write d4 to command" \
			--main-rom work/roms/r45_bebox_bootmain.image \
			--nvram "$OUT/r45-share.nvram" \
			--scsi0-disk "$OUT/r45-share.qcow2" --scsi1-share "$share" \
			--log kbd=debug
		if ls "$OUT"/bebox-share-* >/dev/null 2>&1 \
				|| [ "$(cat "$share/docs/readme.txt")" != "shared with BeOS" ]; then
			echo "FAIL r45-share: volume left behind or folder changed"
			failures=$((failures + 1))
		fi
	else
		echo "skip r45-share (no $R45_DISK or $R45_NVRAM)"
	fi
fi

# 32 bits per pixel on the S3: the Screen preferences switch and the
# confirmation is clicked (the hardware cursor, which the mouse script
# follows, must be where BeOS has the pointer), so the mode is kept past
# the revert timeout
if [ -z "$CASES" ] || want dr8-s3-32bpp; then
	if [ -f "$DR8_DISK" ]; then
		overlay "$DR8_DISK" "$OUT/dr8-s3-32bpp.qcow2"
		# BeBox, NewDisk, preferences, Screen; Colors: 32-Bits/Pixel; OK
		F1=" " run dr8-s3-32bpp 60 "display: format 6, 640x480" \
			--scsi0-disk "$OUT/dr8-s3-32bpp.qcow2" \
			--dclick 30000:33,45 --dclick 33000:120,105 \
			--dclick 36000:332,120 --dclick 39000:273,198 \
			--press 44000:342,81 --release 45500:349,111 \
			--click 47500:318,254
	else
		echo "skip dr8-s3-32bpp (no $DR8_DISK)"
	fi
fi
if [ -z "$CASES" ] || want r45-s3-32bpp; then
	if [ -f "$R45_DISK" ] && [ -f "$R45_NVRAM" ]; then
		overlay "$R45_DISK" "$OUT/r45-s3-32bpp.qcow2"
		cp "$R45_NVRAM" "$OUT/r45-s3-32bpp.nvram"
		# Be menu > Preferences, Screen; Colors: 32 Bits/Pixel; Apply; Keep
		F1=" " run r45-s3-32bpp 92 "display: format 6, 640x480" \
			--main-rom work/roms/r45_bebox_bootmain.image \
			--nvram "$OUT/r45-s3-32bpp.nvram" \
			--scsi0-disk "$OUT/r45-s3-32bpp.qcow2" \
			--click 58000:588,10 --click 60000:455,150 \
			--type 63000:Screen --dclick 65000:142,139 \
			--click 70000:433,215 --click 72000:440,261 \
			--click 74000:430,273 --click 76500:421,265
	else
		echo "skip r45-s3-32bpp (no $R45_DISK or $R45_NVRAM)"
	fi
fi

# The Hobbit prototype: its kernel (loaded by the emulator, no boot ROM)
# reports on serial 1; the window server and the shell come up. The
# November 1994 builds start "Browser", which those disks have as
# "browser", so the shell comes up instead (as on the real machines).
hobbit_case() {
	name=$1; image=$2; seconds=$3; markers=$4; shift 4
	[ -z "$CASES" ] || want $name || return
	if [ ! -f "$image" ]; then
		echo "skip $name (no $image)"
		return
	fi
	overlay "$image" "$OUT/$name.qcow2"
	F1=" " run $name $seconds "$markers" --model hobbit --ram 16 \
		--scsi0-disk "$OUT/$name.qcow2" --serial1 stdout --serial4 null \
		"$@"
}
hobbit_case hobbit-pixels "$HOBBIT_DIR/Hard Drive Image/hobbit-930800009.img" 30 \
	"Volume 'pixels'|Mouse enabled successfully|Starting pixels:/bin/newsh|display: format 2, 640x480"
hobbit_case hobbit-be "$HOBBIT_DIR/Hard Drive Image/hobbit-9308000058.img" 30 \
	"Volume 'be'|Starting be:/bin/newsh|display: format 2, 800x600"
hobbit_case hobbit-be3 "$HOBBIT_DIR/Hard Drive Image/Hobbit-9308000044.img" 30 \
	"Volume 'be3'|Starting be3:/system/gr_server|Finished Bootscript|display: format 2, 800x600"
# the installer floppy (read through the board's floppy DMA)
if [ -z "$CASES" ] || want hobbit-floppy; then
	floppy="$HOBBIT_DIR/Floppy Images/installer.img"
	if [ -f "$floppy" ]; then
		overlay "$floppy" "$OUT/hobbit-floppy-fd.qcow2"
		hobbit_case hobbit-floppy \
			"$HOBBIT_DIR/Hard Drive Image/hobbit-930800009.img" 40 \
			"Volume 'fd' w/size 2880|Starting fd:/system/app_server" \
			--floppy "$OUT/hobbit-floppy-fd.qcow2"
	else
		echo "skip hobbit-floppy (no $floppy)"
	fi
fi

echo "$failures failures"
[ $failures -eq 0 ]
