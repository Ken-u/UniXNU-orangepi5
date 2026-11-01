# UniXNU Orange Pi 5

[UniXNU](https://unixnu.com) device repository for the Orange Pi 5 (Rockchip RK3588S) with an HDMI display and an
M.2 NVMe drive. The goal is the iPadOS 17.7.11 (21H461) userland on a source-built XNU, started from the
[edk2-rk3588](https://github.com/edk2-porting/edk2-rk3588) based firmware of `uefi-orangepi5`.

**Status: host side only.** The configuration resolves, the kernel with the NVMe driver builds and links, and the payload and the
NVMe image are assembled and checked on the host. Nothing has run on an Orange Pi 5 yet; see [`docs/bring-up.md`](docs/bring-up.md)
for the plan and the open inputs, `uefi-orangepi5/docs/first-boot.md` for the procedure on the board, and `porting.toml` for the
per-area record. The SoC support is shared with the EVB10 port, which runs on hardware.

| Repository | Role |
| --- | --- |
| `UniXNU-orangepi5` (this one) | Board configuration, device tree, packaging |
| [`UniXNU-rk3588-common`](https://github.com/Ken-u/UniXNU-rk3588-common) | Shared RK3588 SoC support: CPU, GIC, console, PSCI |
| [`UniXNU-evb10`](https://github.com/Ken-u/UniXNU-evb10) | The EVB10 port; display, GPU, DVFS and eMMC code to reuse |
| `uefi-orangepi5` | Firmware and board tools |

## Contents

| Path | Contents |
| --- | --- |
| `board.toml`, `feeds.toml`, `unixnu.lock` | Board configuration and pinned sources |
| `porting.toml` | Porting record: per-area status, findings and evidence |
| `configs/orangepi5_defconfig` | Console kernel with NVMe storage (no display, no GPU) |
| `package/Rk3588Nvme/` | NVMe driver: PCIe bring-up, polled NVMe host, root and Data windows; host tests |
| `dts/orangepi5.dts` | Board device tree on the shared `rk3588.dtsi` |
| `patches/xnu/` | Board XNU patches and their `series`: the NVMe personality, and the EVB10 patches vendored with their source commit (`VENDORED.md`) |
| `scripts/build-orangepi5-kernel.sh` | Kernel build driver |
| `scripts/build-orangepi5-drivers.py`, `bind-nvme-storage.py` | Compile the NVMe driver against the kernel's compile record; bind it to the original `IOBlockStorageDevice` |
| `docs/bring-up.md` | Hardware inventory, boot flow, storage driver, milestones, open inputs |

## Build

Build with the [UniXNU CLI](https://unixnu.com/building) (Python 3.11 or newer). Settings of the machine that builds go
into an ignored `.env` (see `.env.example`); CMake 3.19.6 or newer is required.

```sh
cd ../UniXNU
./unixnu defconfig orangepi5_defconfig -D ../UniXNU-orangepi5
./unixnu fetch -D ../UniXNU-orangepi5
./unixnu tree -D ../UniXNU-orangepi5
# or the whole kernel:
../UniXNU-orangepi5/scripts/build-orangepi5-kernel.sh
```

To work against a local SoC checkout copy `local.toml.example` to `local.toml`.

## License

See `LICENSE`.
