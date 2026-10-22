#!/bin/sh
# ThreadSanitizer run of the threaded mode (each emulated CPU on a host
# thread of its own): cpu-unit-tests (lwarx/stwcx. from two threads) and an
# R4.5 boot at 133 MHz with --threads, from a throwaway overlay of the
# installed disk.
#
# Races on guest memory (the RAM BeBoxMachine::_SetupMemory allocates, the
# memory of cpu-unit-tests' threaded test) are the guest's own: both CPUs
# touch the same words, as on a real BeBox. They are counted; any other
# race (the emulator's own state) fails the run.
#
#   tests/tsan.sh [SECONDS]      emulated seconds of the boot (default 30;
#                                ThreadSanitizer runs it about 6 times
#                                slower than real time)
#   R45_DISK, R45_NVRAM, R45_ROM override work/disks/r45.qcow2,
#   work/disks/r45.nvram, work/roms/r45_bebox_bootmain.image

cd "$(dirname "$0")/.." || exit 1
SECONDS_RUN=${1:-30}
BIN=tests/linux/out/tsan
OUT=${OUT:-tests/linux/out/tsan-run}
R45_DISK=${R45_DISK:-work/disks/r45.qcow2}
R45_NVRAM=${R45_NVRAM:-work/disks/r45.nvram}
R45_ROM=${R45_ROM:-work/roms/r45_bebox_bootmain.image}

make -C tests/linux -j"$(nproc)" TSAN=1 out/tsan/cpu-unit-tests \
	out/tsan/bebox-headless out/tsan/bebox-img out/tsan/systest.bin \
	out/tsan/smptest.bin >/dev/null || exit 1
mkdir -p "$OUT"
export TSAN_OPTIONS="history_size=4 $TSAN_OPTIONS"

# Prints the number of guest memory races and the other reports; true if
# there are none of those.
classify() {
	python3 -I - "$1" <<'EOF'
import sys
text = open(sys.argv[1], errors="replace").read()
reports = text.split("WARNING: ThreadSanitizer: ")[1:]
guest = 0
other = []
for report in reports:
	report = report.split("SUMMARY: ThreadSanitizer")[0]
	location = report.split("Location is", 1)
	if len(location) == 2 and ("BeBoxMachine::_SetupMemory" in location[1]
			or "ThreadedSmpTests" in location[1]):
		guest += 1
	else:
		other.append(report)
print("  %d race(s) on guest memory, %d other report(s)" % (guest,
	len(other)))
for report in other:
	print("WARNING: ThreadSanitizer: " + report)
sys.exit(1 if other else 0)
EOF
}

failures=0
echo "== cpu-unit-tests"
$BIN/cpu-unit-tests $BIN >"$OUT/cpu-unit-tests.log" 2>&1
grep -q " 0 failures" "$OUT/cpu-unit-tests.log" \
	|| { echo "FAIL cpu-unit-tests (see $OUT/cpu-unit-tests.log)"; \
		failures=$((failures + 1)); }
classify "$OUT/cpu-unit-tests.log" || failures=$((failures + 1))

echo "== R4.5 at 133 MHz, --threads, $SECONDS_RUN s"
if [ -f "$R45_DISK" ] && [ -f "$R45_NVRAM" ] && [ -f "$R45_ROM" ]; then
	rm -f "$OUT/r45.qcow2"
	$BIN/bebox-img create -b "$(realpath "$R45_DISK")" "$OUT/r45.qcow2" \
		>/dev/null || exit 1
	cp "$R45_NVRAM" "$OUT/r45.nvram"
	$BIN/bebox-headless --threads --rom-dir ROMs --model 133 \
		--seconds "$SECONDS_RUN" --main-rom "$R45_ROM" \
		--nvram "$OUT/r45.nvram" --scsi0-disk "$OUT/r45.qcow2" \
		>"$OUT/r45.log" 2>&1
	grep -q "^emulated" "$OUT/r45.log" \
		|| { echo "FAIL the boot did not run (see $OUT/r45.log)"; \
			failures=$((failures + 1)); }
	classify "$OUT/r45.log" || failures=$((failures + 1))
	rm -f "$OUT/r45.qcow2"
else
	echo "skip (no $R45_DISK, $R45_NVRAM or $R45_ROM)"
fi

echo "$failures failures"
[ $failures -eq 0 ]
