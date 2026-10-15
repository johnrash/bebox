#!/bin/bash
# vm-test.sh user@haiku-host
#
# End-to-end tests of the BeBox app on a Haiku machine, run from Linux:
# syncs and builds the app there, copies the boot ROMs and a test disk,
# drives the app with hey (its scripting properties and commands) and
# checks the replies, screenshots and the saved settings. The app's
# settings folder on the Haiku machine is backed up and restored.
#
# Needs here: ROMs/ (bootnub.rom, bootmain.rom) and work/disks/dr8.qcow2
# (DR8 installed); for the typing test also the R3 CD image and
# work/roms/r3_bebox_bootmain.image (tools/extract-rom-images.sh); for the
# sound test work/disks/r45.qcow2 and r45.nvram (R4.5 installed) and
# work/roms/r45_bebox_bootmain.image. The pointer and sound tests need
# BeNC's sendinput on the Haiku machine.
set -u
cd "$(dirname "$0")/.."
VM=${1:?usage: vm-test.sh user@haiku-host}
R3_CD=${R3_CD:-work/media/R3/BeOS Release 3 PowerPC.iso}
R45_DISK=${R45_DISK:-work/disks/r45.qcow2}
R45_NVRAM=${R45_NVRAM:-work/disks/r45.nvram}
SENDINPUT=/boot/home/BeNC/tests/haiku/sendinput
OUT=tests/linux/out/vm
mkdir -p "$OUT"

PASSED=0
FAILED=0

vm() {
	ssh -o BatchMode=yes "$VM" "$1"
}

cleanup() {
	vm 'hey -s BeBox quit >/dev/null 2>&1; sleep 2;
		S=~/config/settings/BeBox; rm -rf "$S";
		[ -d "$S.vmtest-backup" ] && mv "$S.vmtest-backup" "$S"; true' \
		>/dev/null 2>&1
}
trap cleanup EXIT

result() {
	if [ "$2" = ok ]; then
		PASSED=$((PASSED + 1))
	else
		FAILED=$((FAILED + 1))
	fi
	printf "%-32s %s\n" "$1" "$2"
}

check() {
	if [ "$2" = "$3" ]; then
		result "$1" ok
	else
		result "$1" "FAILED (expected \"$2\", got \"$3\")"
	fi
}

# check_at_least NAME MIN VALUE (numbers, decimals allowed)
check_at_least() {
	if awk -v v="$3" -v m="$2" 'BEGIN { exit !(v + 0 >= m + 0) }'; then
		result "$1" ok
	else
		result "$1" "FAILED (expected >= $2, got \"$3\")"
	fi
}

# get PROPERTY [SPECIFIER...]: first "result" of the machine window
get() {
	vm "hey BeBox get $* of Window BeBox" \
		| sed -n 's/.*"result" ([A-Z_0-9]*) : //p' | head -1 \
		| sed 's/^"\(.*\)"$/\1/; s/ (0x[0-9a-fA-F]*)$//; s/^TRUE$/1/;
			s/^FALSE$/0/'
}

start_app() {
	vm "cd ~/BeBox && (./objects.x86_64-cc13-release/BeBox $* \
		> /tmp/bebox-vmtest.log 2>&1 &) ; waitfor 'w>BeBox'; sleep 1"
}

quit_app() {
	vm 'hey -s BeBox quit; for i in 1 2 3 4 5; do
		roster | grep -q BeBox-Emulator || break; sleep 1; done'
}

echo "== build on $VM"
bash ~/.claude/skills/haiku-dev/scripts/sync-to-haiku.sh "$VM" BeBox \
	Makefile BeBox.rdef artwork src >/dev/null || exit 1
vm 'cd ~/BeBox && make -j4 2>&1 | grep -E "error|warning"; true'
vm 'mkdir -p ~/BeBox/ROMs ~/BeBox/test'
scp -q -o BatchMode=yes ROMs/bootnub.rom ROMs/bootmain.rom "$VM:BeBox/ROMs/"
scp -q -o BatchMode=yes work/disks/dr8.qcow2 "$VM:BeBox/test/dr8.qcow2"
vm 'hey -s BeBox quit >/dev/null 2>&1; sleep 1;
	S=~/config/settings/BeBox; rm -rf "$S.vmtest-backup";
	[ -d "$S" ] && mv "$S" "$S.vmtest-backup"; true'

echo "== power and status"
start_app --rom-dir /boot/home/BeBox/ROMs --video s3 \
	--scsi0-disk /boot/home/BeBox/test/dr8.qcow2
check "starts powered off" 0 "$(get Running)"
check "first profile is Default" Default "$(get Profile)"
vm 'hey -s BeBox set Running of Window BeBox to true'
check "Running set true" 1 "$(get Running)"
sleep 45
check_at_least "emulated time after 45 s" 30 "$(get EmulatedTime)"
check_at_least "speed (percent)" 50 "$(get Speed)"
check_at_least "MIPS" 20 "$(get Mips)"
levels=$(vm "hey BeBox get LedLevels of Window BeBox" | grep -c '"result"')
check "LedLevels has two columns" 2 "$levels"
check "DiskLed readable" ok "$(get DiskLed | grep -q -E '^[01]$' && echo ok)"

echo "== captured pointer, VNC style (absolute positions)"
# BeNC's sendinput posts pointer events with positions only, like a VNC
# viewer's pointer or a tablet: 5 moves of 5 px up must move BeOS's cursor
# about 25 px, not jump.
if vm "test -x $SENDINPUT"; then
	pointer_y() {
		vm "hey BeBox get Pointer of Window BeBox" \
			| sed -n 's/.*BPoint([-0-9.]*, \([-0-9.]*\)).*/\1/p'
	}
	vm "$SENDINPUT application/x-vnd.BeBox-Emulator BeBox view:screen \
		press:420,340,1 release:420,340"
	sleep 1
	before=$(pointer_y)
	vm "$SENDINPUT application/x-vnd.BeBox-Emulator BeBox view:screen \
		move:420,340 move:420,335 move:420,330 move:420,325 move:420,320 \
		move:420,315"
	sleep 1
	after=$(pointer_y)
	moved=$(awk -v a="$before" -v b="$after" 'BEGIN { print a - b }')
	if awk -v m="$moved" 'BEGIN { exit !(m >= 15 && m <= 40) }'; then
		result "pointer moves 1:1 when captured" ok
	else
		result "pointer moves 1:1 when captured" \
			"FAILED (moved $moved px for 25: $before -> $after)"
	fi
else
	echo "skip pointer test (no $SENDINPUT)"
fi

echo "== screenshot, serial and floppy"
vm 'rm -f /tmp/bebox-vmtest.png'
saved=$(vm "hey BeBox shot of Window BeBox with path=/tmp/bebox-vmtest.png" \
	| sed -n 's/.*"path" (B_STRING_TYPE) : "\(.*\)"/\1/p')
check "shot replies with the path" /tmp/bebox-vmtest.png "$saved"
scp -q -o BatchMode=yes "$VM:/tmp/bebox-vmtest.png" "$OUT/dr8-desktop.png"
size=$(stat -c %s "$OUT/dr8-desktop.png" 2>/dev/null || echo 0)
check_at_least "screenshot PNG size" 2000 "$size"
check "SerialLog 4 (console)" ok "$(vm "hey BeBox get SerialLog 4 of \
	Window BeBox" | grep -q '"error" (B_INT32_TYPE) : 0' && echo ok)"
vm 'hey -s BeBox set Floppy of Window BeBox to "/boot/home/BeBox/test/none.img"'
check "Floppy set" /boot/home/BeBox/test/none.img "$(get Floppy)"

echo "== settings persist"
vm 'hey -s BeBox set Running of Window BeBox to false'
check "Running set false" 0 "$(get Running)"
vm "hey BeBox shot of Window BeBox with path=/tmp/bebox-off.png" >/dev/null
scp -q -o BatchMode=yes "$VM:/tmp/bebox-off.png" "$OUT/powered-off.png"
check "black screen when powered off" ok "$(python3 -I -c '
import sys
from PIL import Image
im = Image.open(sys.argv[1]).convert("RGB")
print("ok" if im.getextrema() == ((0, 0), (0, 0), (0, 0)) else "not black")
' "$OUT/powered-off.png" 2>&1)"
quit_app
check "settings saved" ok "$(vm 'test -s ~/config/settings/BeBox/settings \
	&& echo ok')"
start_app
check "Floppy kept after restart" /boot/home/BeBox/test/none.img \
	"$(get Floppy)"
vm "hey -s BeBox set Floppy of Window BeBox to '\"\"'"
check "Floppy ejected" "" "$(get Floppy)"
quit_app

if [ -f "$R3_CD" ] && [ -f work/roms/r3_bebox_bootmain.image ]; then
	echo "== typing (R3 CD with its boot ROM)"
	vm 'test -s ~/BeBox/test/R3.iso' || scp -q -o BatchMode=yes "$R3_CD" \
		"$VM:BeBox/test/R3.iso"
	scp -q -o BatchMode=yes work/roms/r3_bebox_bootmain.image \
		"$VM:BeBox/test/r3.rom"
	start_app --power-on --boot-rom /boot/home/BeBox/test/r3.rom \
		--scsi0-disk "''" --scsi3-cd /boot/home/BeBox/test/R3.iso
	sleep 35
	vm 'rm -f /tmp/bebox-license.png /tmp/bebox-booted.png'
	rm -f "$OUT/r3-license.png" "$OUT/r3-booted.png"
	vm "hey BeBox shot of Window BeBox with path=/tmp/bebox-license.png" \
		>/dev/null
	vm "hey BeBox type of Window BeBox with text=\$'\\n'" >/dev/null
	# Return accepts the license agreement (the default button)
	sleep 40
	vm "hey BeBox shot of Window BeBox with path=/tmp/bebox-booted.png" \
		>/dev/null
	scp -q -o BatchMode=yes "$VM:/tmp/bebox-license.png" "$OUT/r3-license.png"
	scp -q -o BatchMode=yes "$VM:/tmp/bebox-booted.png" "$OUT/r3-booted.png"
	if [ ! -s "$OUT/r3-license.png" ] || [ ! -s "$OUT/r3-booted.png" ]; then
		result "typed Return reaches BeOS" "FAILED (no screenshots)"
	elif cmp -s "$OUT/r3-license.png" "$OUT/r3-booted.png"; then
		result "typed Return reaches BeOS" "FAILED (screen unchanged)"
	else
		result "typed Return reaches BeOS" ok
	fi
	quit_app
else
	echo "skip typing test (no $R3_CD or R3 ROM image)"
fi

if [ -f "$R45_DISK" ] && [ -f "$R45_NVRAM" ] \
	&& [ -f work/roms/r45_bebox_bootmain.image ] \
	&& vm "test -x $SENDINPUT"; then
	echo "== sound (R4.5 plays BeStartup.aiff with MediaPlayer), Ctrl+Alt+Delete"
	# The sound output must keep up for the whole sound: no underruns
	# (they are gaps) while the host's sound clock and the emulation's
	# clock drift apart.
	vm 'test -s ~/BeBox/test/r45.qcow2' || scp -q -o BatchMode=yes \
		"$R45_DISK" "$VM:BeBox/test/r45.qcow2"
	scp -q -o BatchMode=yes work/roms/r45_bebox_bootmain.image \
		"$VM:BeBox/test/r45.rom"
	# a profile of its own, with R4.5's NVRAM (boot volume)
	vm "mkdir -p 'config/settings/BeBox/Profiles/R4.5 sound'"
	scp -q -o BatchMode=yes "$R45_NVRAM" \
		"$VM:config/settings/BeBox/Profiles/R4.5 sound/nvram.bin"
	scp -q -o BatchMode=yes tests/haiku/guest-click.sh "$VM:BeBox/test/"
	vm 'cp ~/BeBox/test/r45.qcow2 /tmp/bebox-sound.qcow2;
		rm -f /tmp/bebox-guest-click.pos'
	start_app --profile "'R4.5 sound'" --power-on \
		--boot-rom /boot/home/BeBox/test/r45.rom \
		--scsi0-disk /tmp/bebox-sound.qcow2 --scsi3-cd "''"
	check "--profile makes a profile" "R4.5 sound" "$(get Profile)"
	sleep 50
	click() {
		vm "bash ~/BeBox/test/guest-click.sh $*" >/dev/null
	}
	# capture the mouse; Be menu > Applications, then open Terminal
	vm "$SENDINPUT application/x-vnd.BeBox-Emulator BeBox view:screen \
		press:400,300,1 wait:60 release:400,300"
	sleep 1
	click 588 10
	sleep 2
	click 455 112
	sleep 4
	vm "hey BeBox type of Window BeBox with text=Terminal" >/dev/null
	sleep 2
	click 145 239 dclick
	sleep 6
	vm "hey BeBox type of Window BeBox with \
		text=\$'/boot/beos/apps/MediaPlayer /boot/beos/etc/sounds/BeStartup.aiff &\\n'" \
		>/dev/null
	sleep 12
	stats=$(get AudioStats)
	echo "   $stats"
	received=$(echo "$stats" | sed -n 's/.*received \([0-9]*\).*/\1/p')
	underruns=$(echo "$stats" | sed -n 's/.*underruns \([0-9]*\).*/\1/p')
	check_at_least "sound frames played (5 s)" 220500 "${received:-0}"
	check "sound has no gaps (underruns)" 0 "${underruns:-none}"
	check_at_least "speed while playing (percent)" 95 "$(get Speed)"
	# Haiku keeps Ctrl+Alt+Delete, the menu command sends it: R4.5 shows
	# its Team Monitor
	vm "rm -f /tmp/bebox-cad-before.png /tmp/bebox-cad-after.png; \
		hey BeBox shot of Window BeBox with path=/tmp/bebox-cad-before.png; \
		hey BeBox cadl of Window BeBox; sleep 3; \
		hey BeBox shot of Window BeBox with path=/tmp/bebox-cad-after.png" \
		>/dev/null
	rm -f "$OUT/r45-before-cad.png" "$OUT/r45-team-monitor.png"
	scp -q -o BatchMode=yes "$VM:/tmp/bebox-cad-before.png" \
		"$OUT/r45-before-cad.png"
	scp -q -o BatchMode=yes "$VM:/tmp/bebox-cad-after.png" \
		"$OUT/r45-team-monitor.png"
	if [ ! -s "$OUT/r45-before-cad.png" ] \
		|| [ ! -s "$OUT/r45-team-monitor.png" ]; then
		result "Ctrl+Alt+Delete reaches BeOS" "FAILED (no screenshots)"
	elif cmp -s "$OUT/r45-before-cad.png" "$OUT/r45-team-monitor.png"; then
		result "Ctrl+Alt+Delete reaches BeOS" "FAILED (screen unchanged)"
	else
		result "Ctrl+Alt+Delete reaches BeOS" ok
	fi

	echo "== profiles"
	# switching restarts the machine with the other profile's settings
	vm "hey -s BeBox set Floppy of Window BeBox to /boot/home/BeBox/test/p.img"
	vm "hey -s BeBox set Profile of Window BeBox to Default"
	check "switched to Default" Default "$(get Profile)"
	check "running after the switch" 1 "$(get Running)"
	check "Default has its own floppy (none)" "" "$(get Floppy)"
	vm "hey -s BeBox set Profile of Window BeBox to 'R4.5 sound'"
	check "R4.5 sound kept its floppy" /boot/home/BeBox/test/p.img \
		"$(get Floppy)"
	check "two profiles" 2 "$(vm "hey BeBox get Profiles of Window BeBox" \
		| grep -c '"result"')"
	check "switching to a missing profile fails" ok "$(vm "hey BeBox set \
		Profile of Window BeBox to Missing" | grep -q 'error' && echo ok)"
	quit_app
	vm 'rm -f /tmp/bebox-sound.qcow2'
else
	echo "skip sound test (no R4.5 disk, NVRAM or ROM image, or sendinput)"
fi

echo "$PASSED passed, $FAILED failed (screenshots in $OUT)"
[ $FAILED -eq 0 ]
