#!/bin/bash
# run-ppc-oracle.sh [build dir] [cases] [seed] [groups]
# Generates random instruction test cases, runs them under qemu-ppc (603e)
# and under the emulator core (ppc-exec) and compares the results.
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$(cd "${1:-$HERE/out}" && pwd)"
COUNT="${2:-20000}"
SEED="${3:-1}"
GROUPS_ARG="${4:-}"
WORK="$OUT/oracle"
mkdir -p "$WORK"

ARGS=(--seed "$SEED" --count "$COUNT")
[ -n "$GROUPS_ARG" ] && ARGS+=(--groups "$GROUPS_ARG")
python3 -I "$HERE/../../tools/ppc-testgen.py" "${ARGS[@]}" "$WORK/cases.S"

AS=powerpc-linux-gnu-as
LD=powerpc-linux-gnu-ld
$AS -mppc -m603 -o "$WORK/exec.o" "$HERE/ppc/exec.S"
$AS -mppc -m603 -o "$WORK/cases.o" "$WORK/cases.S"
$LD -static -e _start --no-warn-rwx-segments -o "$WORK/exec.elf" \
	"$WORK/exec.o" "$WORK/cases.o"

qemu-ppc -cpu 603e "$WORK/exec.elf" > "$WORK/qemu.bin"
"$OUT/ppc-exec" --model 603e "$WORK/exec.elf" > "$WORK/emu.bin"
python3 -I "$HERE/compare-ppc.py" "$WORK/qemu.bin" "$WORK/emu.bin" \
	"$WORK/cases.S.names" "$WORK/cases.S"
