# adst-2009-libfprint

[![CI](https://github.com/arqueon/adst-2009-libfprint/actions/workflows/ci.yml/badge.svg)](https://github.com/arqueon/adst-2009-libfprint/actions/workflows/ci.yml)
[![AUR](https://img.shields.io/aur/version/libfprint-adst-2009-git)](https://aur.archlinux.org/packages/libfprint-adst-2009-git)
[![License: LGPL-2.1-or-later](https://img.shields.io/badge/License-LGPL--2.1--or--later-blue.svg)](LICENSE)

> **Español:** Lee la documentación completa en [`README.es.md`](README.es.md).

Native Linux **`libfprint-2`** Match-On-Chip driver (`adst2009`) for the **`2009:7638` (`iStorage ADST Fingerprint`)** USB fingerprint sensor (capacitive **BYD `BF5325`** 160×160 @ 508 DPI sensor + **AisinoChip `ACH512`** biometric MCU):

| USB ID | Device String | Sensor / MCU | Mode | `libfprint` Driver | Enroll Stages |
|--------|---------------|--------------|------|--------------------|---------------|
| **`2009:7638`** | `iStorage ADST Fingerprint` | BYD `BF5325` (`FW V1.9`, 160×160 @ 508 DPI) + AisinoChip `ACH512` | **Match-On-Chip (MOC)** | `adst2009` (`FpDevice`) | **3 touches** (on-chip flash slots `1..200`) |

Integrates out-of-the-box with **`fprintd`**, **PAM** (`sudo`, `polkit-1`, GDM/SDDM/greetd, DankMaterialShell, GNOME/KDE lockscreens), and multi-reader setups alongside:
- **[`betterlife-347d-libfprint`](https://github.com/arqueon/betterlife-347d-libfprint)** — Blestech / Shenzhen Betterlife (`347d:0302` MOC & `347d:0304` MOH)
- **[`zk9500-libfprint`](https://github.com/arqueon/zk9500-libfprint)** — ZKTeco ZK9500 (`1b55:0124` optical)
- **[`libfprint-CS9711`](https://github.com/archeYR/libfprint-CS9711)** — Chipsailing CS9711 (`2541:0236` MOH)

---

## Quick Start

### 1. Arch Linux / CachyOS / Manjaro / EndeavourOS (AUR)

Install directly from the official AUR package **`libfprint-adst-2009-git`** (includes `2009:7638` MOC + `CS9711` OpenCV 4/5 support):

```bash
paru -S libfprint-adst-2009-git fprintd
# or:
yay -S libfprint-adst-2009-git fprintd
```

- **OpenCV-free upstream variant (`2009:7638` MOC on vanilla `freedesktop.org/libfprint`):** Build [`packaging/arch-upstream/PKGBUILD`](packaging/arch-upstream/PKGBUILD) — zero OpenCV dependency.
- **4-family unified variant (`ADST 2009:7638` + `Betterlife 347d:0302/0304` + `ZKTeco ZK9500 1b55:0124` + `Chipsailing CS9711 2541:0236`):** Run `./build-pkg.sh` from a clone of this repository and install the generated `.pkg.tar.zst` with `sudo pacman -U`.

### 2. Debian / Ubuntu / Fedora / openSUSE (Generic Linux Installer)

Install build dependencies first:

- **Debian / Ubuntu 24.04+:**
  ```bash
  sudo apt-get update && sudo apt-get install -y \
    build-essential git meson ninja-build pkg-config python3 python3-gi \
    libglib2.0-dev libgusb-dev libusb-1.0-0-dev libpixman-1-dev \
    libnss3-dev libssl-dev libgudev-1.0-dev libudev-dev udev \
    libcairo2-dev fprintd libpam-fprintd
  ```
- **Fedora 40+:**
  ```bash
  sudo dnf install -y gcc gcc-c++ git meson ninja-build pkgconf-pkg-config \
    python3 python3-gobject glib2-devel libgusb-devel libusb1-devel pixman-devel \
    openssl-devel libgudev-devel systemd-devel cairo-devel fprintd fprintd-pam
  ```

Then clone and run the automated installer:

```bash
git clone https://github.com/arqueon/adst-2009-libfprint.git
cd adst-2009-libfprint
# Recommended (no OpenCV required — patches vanilla freedesktop.org/libfprint):
./scripts/install-generic-linux.sh
# Or with Chipsailing CS9711 / SIGFM support (requires cmake + OpenCV dev packages):
./scripts/install-generic-linux.sh --with-cs9711
```

---

## Enrollment, Verification & Multi-Reader CLI (`adst-fprintd`)

When multiple fingerprint sensors are connected at once, standard `fprintd-enroll` / `fprintd-verify` bind strictly to the first D-Bus device (`/net/reactivated/Fprint/Device/0`). This project installs `adst-fprintd` (`fprintd-multi`), which targets the `2009:7638` reader directly by D-Bus name:

```bash
# Show all connected sensors, enrolled fingers, and which sensor PAM currently selects
adst-fprintd status

# Passive read-only USB diagnostic probe (connection test, BF5325/ACH512 FW version, enrolled slots)
adst2009-probe

# Enroll a finger on ADST 2009:7638 (3 touches on first finger; 1 duplicate-check + 3 touches thereafter)
adst-fprintd enroll right-index-finger

# Verify against ADST 2009:7638 on-chip flash template
adst-fprintd verify right-index-finger

# Mirror the same enrolled fingers from another connected reader so PAM selects ADST 2009:7638
adst-fprintd enroll --sync
```

---

## Wire Protocol & Driver Architecture (`2009:7638`)

### 1. Fake USB CD-ROM (`bInterfaceClass = 0x08`) & `usb-storage` Unbind

`2009:7638` enumerates as a USB Mass Storage SCSI Bulk-Only Transport (`0x08 / 0x06 / 0x50`) device (`EP 0x01 OUT`, `EP 0x81 IN`, `wMaxPacketSize = 64`) exposing a virtual CD-ROM (`/dev/sr0`) originally meant to auto-launch a Windows driver installer.

If `usb-storage` remains bound, `udisks2` and the Linux block layer periodically poll `TEST UNIT READY` and issue USB Mass Storage error-recovery resets, colliding with active biometric transactions. [`udev/70-adst-2009-libfprint.rules`](udev/70-adst-2009-libfprint.rules) sets `UDISKS_IGNORE="1"`, keeps USB autosuspend off (`power/control="on"`), and automatically unbinds `usb-storage` on `add|bind`; [`drivers/adst2009/adst2009.c`](drivers/adst2009/adst2009.c) also releases interface `0` with `G_USB_DEVICE_CLAIM_INTERFACE_NONE` so `usb-storage` is never re-attached on close.

### 2. 24-Byte `SZOEMHost` Framing over Vendor SCSI-BOT (`0xEF`)

All biometric commands and responses are fixed **24-byte** little-endian `SZOEMHost` packets wrapped inside standard 31-byte SCSI `CBW` (`USBC`) + 24-byte Data + 13-byte `CSW` (`USBS`) triplets:

- **Write (`CDB = EF 11 00 00 00 00 00 00 18 00`, `CBWFlags = 0x00`):** Sends a 24-byte command packet:
  - `[0..1] prefix = 0xAA55` (`CMD_PREFIX_CODE`, LE: `55 AA`)
  - `[2..3] cmd` (`uint16_t` LE)
  - `[4..5] len` (`uint16_t` LE payload byte length `0..16`)
  - `[6..21] data[16]` (zero-padded parameters)
  - `[22..23] cksum` (`uint16_t` LE 16-bit sum of bytes `[0..21]`)
- **Non-blocking Poll (`CDB = EF 12 00 00 00 00 00 00 18 00`, `CBWFlags = 0x80`):** Reads 24 bytes in `< 2 ms`:
  - While the `ACH512` MCU is busy or waiting for finger contact, `EF 12` immediately returns **`24 × 0xAF`** (`AF AF ... AF`) with `CSW bCSWStatus = 0x00` — it **never** blocks on the USB bus.
  - Once a stage or result is ready, `EF 12` returns a 24-byte response packet (`prefix = 0x55AA`, `rcmd`, `len`, `ret`, `data1`, `data2`, ..., `cksum`).
- **Atomic 3-URB State Engine:** Because every `EF 12` poll finishes in `< 2 ms`, [`adst_bot_run()`](drivers/adst2009/adst2009.c) executes each `CBW → Data → CSW` triplet atomically (`cancellable = NULL`) and checks `GCancellable` between `75 ms` poll intervals, guaranteeing SCSI-BOT phase alignment can never tear mid-transfer while keeping cancellation latency `<= 75 ms`.

### 3. Empirically Verified Command Set & Two-Packet Cancel Drain

| Opcode | Name | Request / Response Behavior on `BF5325 V1.9 / ACH512` |
|--------|------|-------------------------------------------------------|
| `0x0150` | `CMD_TEST_CONNECTION` | `len=0` → `ret=0x0000` |
| `0x0112` | `CMD_GET_FW_VERSION` | `len=0` → `ret=0x0000, data1=0x0901` (`V1.9`: `major = LOBYTE`, `minor = HIBYTE`) |
| `0x0128` | `CMD_GET_ENROLL_COUNT` | `len=0` → `ret=0x0000, data1=<enrolled_count>` |
| `0x0107` | `CMD_GET_EMPTY_ID` | `len=0` → `ret=0x0000, data1=<first_free_slot 1..200>` (or `0x19 ERR_EMPTY_ID_NOEXIST`) |
| `0x0103` | `CMD_ENROLL` | `len=2, data1=<slot_id>` → emits `0xFFF1` (`NEED_FIRST_SWEEP`), `0xFFF4` (`NEED_RELEASE_FINGER`), `0xFFF2` (`NEED_SECOND_SWEEP`), `0xFFF3` (`NEED_THIRD_SWEEP`), and finally `data1=<slot_id>` (`0x18 ERR_DUPLICATION_ID` with `data2=<dup_slot>` if already enrolled; stays active across `0x21 ERR_BAD_QUALITY` retries) |
| `0x0101` | `CMD_VERIFY` | `len=2, data1=<slot_id>` → `0xFFF1` → `ret=0x0000, data1=<slot_id>` (or `0x11 ERR_VERIFY`, `0x13 ERR_TMPL_EMPTY`) |
| `0x0102` | `CMD_IDENTIFY` | `len=0` → `0xFFF1` → `ret=0x0000, data1=<matched_slot_id>` (or `0x12 ERR_IDENTIFY`, immediate `0x15 ERR_ALL_TMPL_EMPTY` when 0 templates exist) |
| `0x0105` | `CMD_CLEAR_TEMPLATE` | `len=2, data1=<slot_id>` → deletes single slot from MCU flash |
| `0x0106` | `CMD_CLEAR_ALLTEMPLATE` | `len=0` → clears all user templates (`fprintd` `clear_storage`) |
| `0x0130` | `CMD_FP_CANCEL` | `len=0` → aborts an active `ENROLL`/`VERIFY`/`IDENTIFY`. Queues **two** `EF 12` responses when interrupting an active command: `(1)` `rcmd=<active_cmd>, ret=1, data1=0x0041 (ERR_FP_CANCEL)`, then `(2)` `rcmd=0x0130, ret=0`, followed by `24 × 0xAF`. [`adst_cancel_drain_step()`](drivers/adst2009/adst2009.c) drains until both `saw_cancel_ack` and `24 × 0xAF` are observed. |

### Hardware Safety Invariant

Bootloader/ISP mode (`0x0171`), flash/firmware write (`0x0005`), destructive factory sensor recalibration (`0x0137`), non-volatile parameter mutation (`0x013A`), and device password lockout (`0x0126`) opcodes are **never** defined or transmitted anywhere in this repository (enforced automatically by CI).

---

## Credits & License

- Copyright (C) 2026 Rubén García Sánchez (`arqueon`).
- Licensed under **GNU Lesser General Public License v2.1 or later (`LGPL-2.1-or-later`)**; see [`LICENSE`](LICENSE).
