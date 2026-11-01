# Vendored patches

These files are copies of `UniXNU-evb10/patches/xnu/` at commit `37befa9162d6b4b080cb63a95cccff0ebb4d34f5`, byte for byte:

- `evb10-exit-trace.patch`
- `evb10-mmap-grant.patch`
- `evb10-thread-sample.patch`
- `evb10-userland-mounts.patch` (generated there from `patches/tools/evb10-userland-mounts.c.inc` by `patches/tools/make-patches.py`)
- `evb10-vc-mirror.patch`

They are not specific to the EVB10 (the Data volume, the union mount of the SystemOS cryptex, the owner's test program and the console mirror
are the same on every RK3588 board of this port), but they came from that board and still carry its names (`evb10-*` boot-args, `native_emmc_*`
symbols that `package/Rk3588Nvme` keeps defining). Fix them in the EVB10 repository and copy them here again; `scripts/check-vendored-patches.sh`
compares the two copies. The proper home for them is the SoC repository (`UniXNU-rk3588-common`), once the EVB10 stops being the only user of its names.
