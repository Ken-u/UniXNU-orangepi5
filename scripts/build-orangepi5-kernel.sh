#!/bin/bash
# Build the Orange Pi 5 kernel in the two passes the display archive needs.
#
#   scripts/build-orangepi5-kernel.sh                  # pass A when needed, then pass B
#   DISPLAY_BUILD=0 scripts/build-orangepi5-kernel.sh  # only the display-less kernel
#
# Pass A builds configs/orangepi5_defconfig (no display). Pass B builds configs/orangepi5_display_defconfig: its archive step
# (scripts/build-orangepi5-display.py) reads the original IOMobileGraphicsFamily code from the kernel of pass A. Pass A is skipped when the
# previous kernel already carries that family.
#
# Settings come from .env (see .env.example): UNIXNU, UNIXNU_WRAP, UNIXNU_LLVM_OBJCOPY, JOBS.
set -euo pipefail

DEVICE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
[ -f "$DEVICE/.env" ] && set -a && . "$DEVICE/.env" && set +a
: "${UNIXNU:?set UNIXNU in .env to the UniXNU checkout}"
JOBS="${JOBS:-8}"
# AvailabilityVersions needs CMake >= 3.19.6; on Ubuntu 20.04 the distribution cmake is 3.16, so prefer /usr/local/bin.
[ -x /usr/local/bin/cmake ] && export PATH="/usr/local/bin:$PATH"
WRAP="${UNIXNU_WRAP:-}"
[ -n "${UNIXNU_LLVM_OBJCOPY:-}" ] && export UNIXNU_LLVM_OBJCOPY

unixnu() { (cd "$UNIXNU" && $WRAP ./unixnu "$@" -D "$DEVICE"); }
OUT="$UNIXNU/out/orangepi5/ipados-17.7.11-21H461/orangepi5"                            # orangepi5_defconfig (pass A)
OUT_DISPLAY="$UNIXNU/out/orangepi5/ipados-17.7.11-21H461/orangepi5_display"            # orangepi5_display_defconfig (pass B)
DISPLAY_BUILD="${DISPLAY_BUILD:-1}"

# Asm in patches and drivers: the target is Mach-O and the Darwin assembler treats ';' as a comment, so
# "dsb ishst; msr x, %0; isb" assembles to a lone dsb. Separate instructions with "\n\t".
semicolon='(__asm__|[^_[:alnum:]]asm)[[:space:]]*(volatile)?[[:space:]]*\([[:space:]]*"[^"]*;'
bad="$( { grep -nE "^\\+.*$semicolon" "$DEVICE"/patches/xnu/*.patch 2>/dev/null || true; \
          grep -nE "$semicolon" "$DEVICE"/package/*/*.cpp "$DEVICE"/package/*/*.h "$DEVICE"/package/*.c 2>/dev/null || true; } )"
if [ -n "$bad" ]; then echo "$bad" >&2; echo "error: ';' inside inline assembly" >&2; exit 1; fi

build_pass() { # defconfig outdir
	echo "== configure $1"
	unixnu defconfig "$1"
	# make does not track the force-loaded archives: remove the linked kernel so the link always runs.
	rm -f "$2"/tree/build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837.unstripped.noctf
	echo "== build kernel ($1)"
	unixnu build kernel -j "$JOBS"
}

PLAN="$OUT/kernel/kernel.json"
if [ "$DISPLAY_BUILD" = 1 ]; then
	if [ ! -f "$PLAN" ] || ! grep -q 'com.apple.iokit.IOMobileGraphicsFamily' "$PLAN"; then
		build_pass orangepi5_defconfig "$OUT"
	fi
	build_pass orangepi5_display_defconfig "$OUT_DISPLAY"
	FINAL="$OUT_DISPLAY"
else
	build_pass orangepi5_defconfig "$OUT"
	FINAL="$OUT"
fi
# The addresses NativeNvmeBlock was bound to are only valid for the original IOStorageFamily they were audited in: check the linked kernel.
if [ -f "$FINAL/kernel/kernel.json" ] && [ -f "$FINAL/tree/scripts/bind-nvme-storage.py" ]; then
	python3 "$FINAL/tree/scripts/bind-nvme-storage.py" --verify-kernel "$FINAL/kernel/kernel.json"
fi
echo "kernel: $FINAL/kernel/kernel.macho"
