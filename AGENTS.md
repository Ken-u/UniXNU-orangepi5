# Agent instructions

This repository owns the Orange Pi 5 board integration: device tree, memory layout, NVMe root, HDMI display and
packaging. Shared RK3588 resources are in `UniXNU-rk3588-common`. Keep board facts here and SoC facts in the SoC
repository. Code and idioms to reuse (display, GPU, DVFS, eMMC model for the NVMe driver) are in `UniXNU-evb10`.

Read these UniXNU pages before editing:

1. [Porting](https://unixnu.com/porting/)
2. [Device repositories](https://unixnu.com/porting/device-repository)
3. [Device trees](https://unixnu.com/porting/device-tree)
4. [Porting records](https://unixnu.com/porting/records)

Rules specific to this repository:

- Only catalog components go into `[components] include`. Board drivers live under `package/`.
- The target is Mach-O; inline assembly goes through the Darwin assembler, where `;` starts a comment.
  `scripts/build-orangepi5-kernel.sh` rejects it in patches.
- Do not commit firmware, vendor binaries, generated includes, IPSW or Apple content, logs, images or build output.
- Addresses, hostnames, accounts and local paths of the machines that build and run the board belong in the ignored
  `.env`, never in the repository.
- Update `porting.toml` findings or evidence in the same change as the code. Record `host-verified` or
  `device-verified` only with a command, a full commit and an accessible log. Nothing here is device-verified yet.
- The board has no remote power switch. A change that can hang the firmware needs the user to pull power.
- `patches/xnu/evb10-*.patch` are copies of the EVB10 patches (`patches/xnu/VENDORED.md` names the commit). Fix them in
  `UniXNU-evb10` first and copy; `scripts/check-vendored-patches.sh` shows the drift.
- `package/Rk3588Nvme` follows `Rk3588Emmc` (see its `AGENTS.md`). Run its host tests before every commit that touches it.

Run builds from the UniXNU checkout:

```sh
./unixnu defconfig orangepi5_defconfig -D ../UniXNU-orangepi5
./unixnu tree -D ../UniXNU-orangepi5
python3 scripts/porting-record.py check --device ../UniXNU-orangepi5
```

Report commands, results, required local inputs, and hardware checkpoints. Write concise English describing current
behavior.
