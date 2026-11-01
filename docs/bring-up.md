# Orange Pi 5 bring-up

Status: **host side only**. The firmware builds (`uefi-orangepi5`), the device configuration resolves and the
console kernel builds. Nothing has run on an Orange Pi 5 yet; every "expected" below is a plan, not an observation.

## Board

| Part | Orange Pi 5 | Same as the EVB10? |
| --- | --- | --- |
| SoC | RK3588S (8 cores, 4 x A76 + 4 x A55, Mali-G610) | Yes, minus some interfaces |
| Debug console | UART2, 3-pin header, 1.5 Mbaud 8N1 | Yes (UART2, `feb50000`) |
| Root and Data storage | NVMe in the M.2 M-key slot, PCIe 2.0 x1 (`pcie2x1l2`, combphy0, reset GPIO3_D1, 3.3 V enable GPIO0_PC5) | No: the EVB10 uses eMMC |
| eMMC socket, microSD | dwcmshc (as the EVB10 eMMC), dw_mmc | not used |
| Display | HDMI0 from VOP2 video port 0 | No: the EVB10 drives a panel from video port 2 |
| GPU rail | RK806 BUCK1 (`vdd_gpu_s0`) | Yes |
| Big-core rails | RK8602 / RK8603 at 0x42 / 0x43 on I2C0 | Yes |
| Audio | ES8388 on I2C6 0x10 + I2S1, HDMI audio | No |
| RTC | HYM8563 on I2C6 0x51 | No |
| Reset / power | none remote: the user pulls power | No: the EVB10 has a reset controller |

## Boot flow

1. The firmware is the SPI NOR image of `uefi-orangepi5`: TF-A (BL31) at EL3 with PSCI over SMC, OP-TEE, and the UEFI
   volume with the UniXNU modules (`DarwinBoot`, the UniXNU menu and the boot watchdog `UniXNUBootWdtDxe`).
2. UEFI initialises DRAM, PCIe, the NVMe controller and HDMI (GOP). `NvmExpressDxe` exposes the NVMe drive and the
   firmware finds the ESP on it.
3. The ESP carries the payload: `kernel.macho`, the trust caches and `EFI/UniXNU/unixnu.toml`. `DarwinBoot` builds
   the ADT and the boot arguments, takes `owned_memory_size` from the UEFI memory map and jumps to the kernel at EL1; the watchdog driver
   of the firmware armed itself earlier.
4. The kernel keeps the watchdog fed with `wdt-kick=1800`; a hung kernel ends in a reset about 89 seconds later.
5. The userdata partition behind the ESP holds the OS and SystemOS images and the Data seed; the NVMe block driver
   (to be written) publishes them to IOKit.

## Milestones

| | Goal | Needs |
| --- | --- | --- |
| M0 | UEFI runs on the board: serial log, HDMI picture, NVMe drive visible | Board, serial adapter, a way to write the SPI NOR |
| M1 | The kernel starts from the NVMe ESP payload (serial log up to the missing root volume) | M0, RAM size, a payload on the drive |
| M2 | NVMe driver: root and Data mount, `launchd` runs | A written userdata partition |
| M3 | The boot screen on the HDMI monitor through the GOP frame buffer | M1 |
| M4 | VOP2 scanout on video port 0, Mali composition, frame rate | M2, M3 |
| later | USB input (xHCI + HID), audio, Ethernet and Wi-Fi, microSD | |

## Placeholders and open inputs

- `dts/orangepi5.dts` memory node and `payload.dram_size`: 4 GiB at `0x100000000`. Replace with the memory map of
  the first run (the board comes in 4, 8, 16 and 32 GiB).
- `board.toml [userdata]`: a proposed layout (ESP 512 MiB, userdata 64 GiB). Needs the drive size.
- `payload.adt.chip_id`, `payload.adt.owned_memory_size`: copied from the EVB10 port, unchecked here.
- How the SPI NOR image is written: not decided (needs the board).

## NVMe driver plan

Model: `Rk3588Emmc` of the EVB10 port.

- The firmware leaves the PCIe 2.0 x1 link up and the controller BAR assigned; the driver reads them (no link training in the kernel).
- A polled admin queue and one polled I/O queue in non-cacheable or cache-maintained memory, 4 KiB pages, no MSI.
- Block windows declared by the root manifest on the userdata partition: OS and SystemOS read-only, Data writable.
- An IOKit personality patch like `rk3588-emmc-personality.patch` in the SoC repository (to be written when the driver exists).
- Host test of the queue logic with a fake controller, like `tests/test-emmc-host.cpp` of the EVB10 port.

## Build

```sh
source ../host/env.sh        # or the equivalent environment of the machine that builds
scripts/build-orangepi5-kernel.sh
```

The script needs `.env` (see `.env.example`) and CMake 3.19.6 or newer.
