#!/bin/bash
# Build the Orange Pi 5 kernel (configs/orangepi5_defconfig, one pass: console and NVMe storage, no display archive yet).
#
#   scripts/build-orangepi5-kernel.sh
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
OUT="$UNIXNU/out/orangepi5/ipados-17.7.11-21H461/orangepi5"

# Asm in patches and drivers: the target is Mach-O and the Darwin assembler treats ';' as a comment, so
# "dsb ishst; msr x, %0; isb" assembles to a lone dsb. Separate instructions with "\n\t".
semicolon='(__asm__|[^_[:alnum:]]asm)[[:space:]]*(volatile)?[[:space:]]*\([[:space:]]*"[^"]*;'
bad="$( { grep -nE "^\\+.*$semicolon" "$DEVICE"/patches/xnu/*.patch 2>/dev/null || true; \
          grep -nE "$semicolon" "$DEVICE"/package/*/*.cpp "$DEVICE"/package/*/*.h "$DEVICE"/package/*.c 2>/dev/null || true; } )"
if [ -n "$bad" ]; then echo "$bad" >&2; echo "error: ';' inside inline assembly" >&2; exit 1; fi

echo "== configure orangepi5_defconfig"
unixnu defconfig orangepi5_defconfig
# make does not track the force-loaded archives: remove the linked kernel so the link always runs.
rm -f "$OUT"/tree/build/xnu/ios-obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837.unstripped.noctf
echo "== build kernel (orangepi5_defconfig)"
unixnu build kernel -j "$JOBS"
# The addresses NativeNvmeBlock was bound to are only valid for the original IOStorageFamily they were audited in: check the linked kernel.
if [ -f "$OUT/kernel/kernel.json" ] && [ -f "$OUT/tree/scripts/bind-nvme-storage.py" ]; then
	python3 "$OUT/tree/scripts/bind-nvme-storage.py" --verify-kernel "$OUT/kernel/kernel.json"
fi
echo "kernel: $OUT/kernel/kernel.macho"
