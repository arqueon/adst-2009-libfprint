# adst-2009-libfprint

[![CI](https://github.com/arqueon/adst-2009-libfprint/actions/workflows/ci.yml/badge.svg)](https://github.com/arqueon/adst-2009-libfprint/actions/workflows/ci.yml)
[![AUR](https://img.shields.io/aur/version/libfprint-adst-2009-git)](https://aur.archlinux.org/packages/libfprint-adst-2009-git)
[![License: LGPL-2.1-or-later](https://img.shields.io/badge/License-LGPL--2.1--or--later-blue.svg)](LICENSE)

> **English:** Read the English documentation in [`README.md`](README.md).

Driver nativo **`libfprint-2`** Match-On-Chip (`adst2009`) en Linux para el lector USB de huellas **`2009:7638` (`iStorage ADST Fingerprint`)** (sensor capacitivo **BYD `BF5325`** 160×160 @ 508 DPI + microcontrolador biométrico **AisinoChip `ACH512`**):

| ID USB | Descriptor USB | Sensor / MCU | Modo | Driver `libfprint` | Toques / Registro |
|--------|----------------|--------------|------|--------------------|-------------------|
| **`2009:7638`** | `iStorage ADST Fingerprint` | BYD `BF5325` (`FW V1.9`, 160×160 @ 508 DPI) + AisinoChip `ACH512` | **Match-On-Chip (MOC)** | `adst2009` (`FpDevice`) | **3 toques** (flash interna slots `1..200`) |

Compatible de fábrica con **`fprintd`**, **PAM** (`sudo`, `polkit-1`, GDM/SDDM/greetd, DankMaterialShell, bloqueos GNOME/KDE) y configuraciones multi-lector junto con:
- **[`betterlife-347d-libfprint`](https://github.com/arqueon/betterlife-347d-libfprint)** — Blestech / Shenzhen Betterlife (`347d:0302` MOC y `347d:0304` MOH)
- **[`zk9500-libfprint`](https://github.com/arqueon/zk9500-libfprint)** — ZKTeco ZK9500 (`1b55:0124` óptico)
- **[`libfprint-CS9711`](https://github.com/archeYR/libfprint-CS9711)** — Chipsailing CS9711 (`2541:0236` MOH)

---

## Instalación rápida

### 1. Arch Linux / CachyOS / Manjaro / EndeavourOS (AUR)

Instala directamente desde el paquete oficial en AUR **`libfprint-adst-2009-git`** (incluye soporte `2009:7638` MOC + `CS9711` con OpenCV 4/5):

```bash
paru -S libfprint-adst-2009-git fprintd
# o bien:
yay -S libfprint-adst-2009-git fprintd
```

- **Variante sin OpenCV (`2009:7638` MOC sobre `freedesktop.org/libfprint` upstream puro):** compila [`packaging/arch-upstream/PKGBUILD`](packaging/arch-upstream/PKGBUILD).
- **Variante unificada de 4 familias (`ADST 2009:7638` + `Betterlife 347d:0302/0304` + `ZKTeco ZK9500 1b55:0124` + `Chipsailing CS9711 2541:0236`):** ejecuta `./build-pkg.sh` desde un clon de este repositorio e instala el `.pkg.tar.zst` generado con `sudo pacman -U`.

### 2. Debian / Ubuntu / Fedora / openSUSE (Instalador genérico Linux)

Instala primero las dependencias de compilación:

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

Clona el repositorio y lanza el instalador:

```bash
git clone https://github.com/arqueon/adst-2009-libfprint.git
cd adst-2009-libfprint
# Recomendado (sin OpenCV — parchea libfprint upstream puro):
./scripts/install-generic-linux.sh
# O con soporte adicional para Chipsailing CS9711 / SIGFM (requiere cmake + OpenCV):
./scripts/install-generic-linux.sh --with-cs9711
```

---

## Registro, verificación y CLI multi-lector (`adst-fprintd`)

Cuando hay varios lectores de huella conectados simultáneamente, `fprintd-enroll` y `fprintd-verify` usan únicamente el primer dispositivo en D-Bus (`/net/reactivated/Fprint/Device/0`). Este proyecto instala `adst-fprintd` (`fprintd-multi`), que selecciona directamente el lector `2009:7638`:

```bash
# Ver todos los lectores conectados, huellas registradas y cuál está activo en PAM
adst-fprintd status

# Sonda USB pasiva de solo lectura (test de conexión, versión FW BF5325/ACH512 y huellas en flash)
adst2009-probe

# Registrar un dedo en ADST 2009:7638 (3 toques en el primer dedo; 1 chequeo + 3 toques en los siguientes)
adst-fprintd enroll right-index-finger

# Verificar huella contra la memoria flash interna del ADST 2009:7638
adst-fprintd verify right-index-finger

# Sincronizar los mismos dedos registrados en otro lector conectado para darle prioridad en PAM
adst-fprintd enroll --sync
```

---

## Protocolo en cable y arquitectura del driver (`2009:7638`)

### 1. CD-ROM USB virtual (`bInterfaceClass = 0x08`) y desacoplo de `usb-storage`

El `2009:7638` se enumera como un dispositivo USB Mass Storage SCSI Bulk-Only Transport (`0x08 / 0x06 / 0x50`, `EP 0x01 OUT`, `EP 0x81 IN`, `wMaxPacketSize = 64`) que expone una unidad CD-ROM virtual (`/dev/sr0`) concebida originalmente para autoejecutar un instalador en Windows.

Si `usb-storage` permanece enlazado, `udisks2` y la capa de bloques de Linux envían periódicamente `TEST UNIT READY` y resets USB Mass Storage que colisionan con las transferencias biométricas. [`udev/70-adst-2009-libfprint.rules`](udev/70-adst-2009-libfprint.rules) marca `UDISKS_IGNORE="1"`, desactiva el autosuspend USB (`power/control="on"`) y desacopla `usb-storage` automáticamente en `add|bind`; además, [`drivers/adst2009/adst2009.c`](drivers/adst2009/adst2009.c) libera la interfaz `0` con `G_USB_DEVICE_CLAIM_INTERFACE_NONE` para no reconectar jamás `usb-storage` al cerrar el dispositivo.

### 2. Paquetes `SZOEMHost` de 24 bytes sobre SCSI-BOT vendor (`0xEF`)

Todos los comandos y respuestas biométricas son paquetes `SZOEMHost` fijos de **24 bytes** en little-endian encapsulados en tripletes SCSI estándar (`31 B CBW` `USBC` + `24 B Data` + `13 B CSW` `USBS`):

- **Escritura (`CDB = EF 11 00 00 00 00 00 00 18 00`, `CBWFlags = 0x00`):** envía paquete de comando de 24 B (`prefix = 0xAA55`, `cmd`, `len`, `data[16]`, `cksum` suma de 16 bits de los bytes `0..21`).
- **Sondeo no bloqueante (`CDB = EF 12 00 00 00 00 00 00 18 00`, `CBWFlags = 0x80`):** lee 24 B en `< 2 ms`:
  - Mientras el MCU `ACH512` procesa o espera el contacto del dedo, `EF 12` responde inmediatamente con **`24 × 0xAF`** (`CSW bCSWStatus = 0x00`), sin bloquear nunca el bus USB.
  - Cuando hay progreso o resultado listo, `EF 12` devuelve un paquete de 24 B (`prefix = 0x55AA`, `rcmd`, `len`, `ret`, `data1`, `data2`, ..., `cksum`).
- **Motor atómico de 3 URBs:** dado que cada sondeo `EF 12` tarda `< 2 ms`, [`adst_bot_run()`](drivers/adst2009/adst2009.c) ejecuta cada triplete `CBW → Data → CSW` de forma atómica (`cancellable = NULL`) y evalúa `GCancellable` entre intervalos de `75 ms`, evitando cualquier desincronización de fases SCSI-BOT y manteniendo una latencia de cancelación `<= 75 ms`.

### 3. Comandos verificados y drenado doble de cancelación (`0x0130`)

- **`0x0150` (`CMD_TEST_CONNECTION`)**, **`0x0112` (`CMD_GET_FW_VERSION` → `V1.9`)**, **`0x0128` (`CMD_GET_ENROLL_COUNT`)**, **`0x0107` (`CMD_GET_EMPTY_ID`)**.
- **`0x0103` (`CMD_ENROLL`)**: emite `0xFFF1` (`NEED_FIRST_SWEEP`), `0xFFF4` (`NEED_RELEASE_FINGER`), `0xFFF2` (`NEED_SECOND_SWEEP`), `0xFFF3` (`NEED_THIRD_SWEEP`) y finalmente el `slot_id` guardado (`1..200`), permaneciendo activo ante reintentos de calidad (`0x21 ERR_BAD_QUALITY`) y detectando duplicados en chip (`0x18 ERR_DUPLICATION_ID`).
- **`0x0101` (`CMD_VERIFY`)**, **`0x0102` (`CMD_IDENTIFY`)**, **`0x0105` (`CMD_CLEAR_TEMPLATE`)**, **`0x0106` (`CMD_CLEAR_ALLTEMPLATE`)**.
- **`0x0130` (`CMD_FP_CANCEL`)**: al abortar un comando activo en el `ACH512`, el firmware encola **dos** respuestas consecutivas en `EF 12`: `(1)` `rcmd=<cmd_activo>, ret=1, data1=0x0041 (ERR_FP_CANCEL)` y `(2)` `rcmd=0x0130, ret=0`, seguidas por `24 × 0xAF`. [`adst_cancel_drain_step()`](drivers/adst2009/adst2009.c) drena hasta observar tanto el ACK de `0x0130` como `24 × 0xAF`.

### Invariante de seguridad de hardware

Los opcodes de modo bootloader/ISP (`0x0171`), escritura de firmware/flash (`0x0005`), recalibración destructiva de fábrica (`0x0137`), mutación de parámetros no volátiles (`0x013A`) y bloqueo por contraseña de dispositivo (`0x0126`) **jamás** se definen ni transmiten en este repositorio (verificado automáticamente en CI).

---

## Créditos y licencia

- Copyright (C) 2026 Rubén García Sánchez (`arqueon`).
- Distribuido bajo **GNU Lesser General Public License v2.1 o posterior (`LGPL-2.1-or-later`)**; véase [`LICENSE`](LICENSE).
