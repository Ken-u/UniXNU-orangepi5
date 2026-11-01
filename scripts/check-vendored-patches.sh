#!/bin/bash
# Compare the evb10-* patches vendored in patches/xnu with the EVB10 repository next to this one.
#
#   scripts/check-vendored-patches.sh [PATH_TO_UniXNU-evb10]
#
# Exit 0: identical (or the EVB10 checkout is not there, which is said). Exit 1: a difference.
set -euo pipefail
DEVICE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EVB10="${1:-$DEVICE/../UniXNU-evb10}"
if [ ! -d "$EVB10/patches/xnu" ]; then echo "no EVB10 checkout at $EVB10: nothing to compare"; exit 0; fi
status=0
for patch in evb10-exit-trace evb10-mmap-grant evb10-thread-sample evb10-userland-mounts evb10-vc-mirror; do
  if cmp -s "$DEVICE/patches/xnu/$patch.patch" "$EVB10/patches/xnu/$patch.patch"; then
    echo "same:      $patch.patch"
  else
    echo "DIFFERENT: $patch.patch"
    status=1
  fi
done
exit $status
