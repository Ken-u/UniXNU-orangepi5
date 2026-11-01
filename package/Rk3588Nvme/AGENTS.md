# Agent instructions

## Responsibility

Maintain the NVMe driver and block storage of the Orange Pi 5. Shared XNU patches live in `UniXNU-rk3588-common`; board layout, partition windows and
boot arguments live in `UniXNU-orangepi5`. The eMMC original is `UniXNU-evb10/package/Rk3588Emmc`; read-only for this repository.

## Rules

- `NativeNvme.cpp` is derived from `NativeEmmc.cpp`. A fix to the shared parts (GPT scan, root image, Data volume, watchdog feeder, PMGR nub, kernel hooks)
  belongs in both; say so in the commit message. `Gpt.h`, `RootManifest.h` and `StorageCommon.h` are copies of the EVB10's headers with the namespace renamed.
- The C symbols `native_emmc_*` and the boot arguments `emmc-systemos-union`, `emmc-data-mount` and `emmc-data-copy` keep their names: kernel patches that
  are not in this directory use them.
- `NvmeHost.h` has no kernel dependency and is tested by `tests/test-nvme-host.cpp`. Do not change a register access, an ordering, a flush or a timeout
  without a host test that fails without the change, and a device record once the board has run.
- Facts about the PCIe controller (addresses, windows) are from edk2-rk3588 and were not measured. When the board has run, replace the sentence in
  `README.md` and in `porting.toml` with the log lines that show them.
- Writes stay inside the declared Data window. The host test enforces it; keep it passing.
- Inline assembly in Mach-O targets uses `"\n\t"` between instructions; `;` starts a comment.
- Run the two host checks from the README before committing, and report the commands, results and source revisions. Report hardware observations
  separately from host results.
