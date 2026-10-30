#!/usr/bin/env bash
# Generic Meson/Ninja builder and installer for Debian/Ubuntu, Fedora, openSUSE, etc.
# Default: builds against vanilla upstream freedesktop libfprint (no OpenCV required).
# --with-cs9711: builds on top of archeYR/libfprint-CS9711 (SIGFM + OpenCV 4/5).
set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
WORK_DIR="${ADST_WORKDIR:-/tmp/adst-2009-libfprint-generic}"
WITH_CS9711=0
PREFIX="/usr"
LIBDIR=""

usage() {
  cat <<'EOF'
Usage: ./scripts/install-generic-linux.sh [--with-cs9711] [--prefix /usr] [--libdir lib/x86_64-linux-gnu]

Options:
  --with-cs9711         Build against archeYR/libfprint-CS9711 (OpenCV 4/5 SIGFM) instead of
                        vanilla upstream freedesktop libfprint.
  --prefix PATH         Installation prefix (default: /usr so system fprintd picks up libfprint-2.so.2).
  --libdir DIR          Override library subdir if needed (auto-detected via pkg-config otherwise).
  -h, --help            Show this help message.
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --with-cs9711) WITH_CS9711=1; shift ;;
    --prefix) PREFIX="$2"; shift 2 ;;
    --libdir) LIBDIR="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
  esac
done

if [[ -z "${LIBDIR}" ]]; then
  sys_libdir="$(pkg-config --variable=libdir glib-2.0 2>/dev/null || true)"
  if [[ -n "${sys_libdir}" && "${sys_libdir}" == "${PREFIX}/"* ]]; then
    LIBDIR="${sys_libdir#${PREFIX}/}"
  elif [[ -d "/usr/lib/x86_64-linux-gnu" ]]; then
    LIBDIR="lib/x86_64-linux-gnu"
  elif [[ -d "/usr/lib64" && ! -L "/usr/lib64" ]]; then
    LIBDIR="lib64"
  else
    LIBDIR="lib"
  fi
fi

echo "==> Building in ${WORK_DIR} (prefix=${PREFIX}, libdir=${LIBDIR}, with_cs9711=${WITH_CS9711})..."
rm -rf "${WORK_DIR}"
mkdir -p "${WORK_DIR}"

if [[ "${WITH_CS9711}" -eq 1 ]]; then
  git clone --depth 1 --branch cs9711-rebase https://github.com/archeYR/libfprint-CS9711.git "${WORK_DIR}/libfprint"
  patch -d "${WORK_DIR}/libfprint" -Np1 -i "${REPO_DIR}/patches/0001-adst2009-cs9711.patch"
else
  git clone --depth 1 https://gitlab.freedesktop.org/libfprint/libfprint.git "${WORK_DIR}/libfprint"
  patch -d "${WORK_DIR}/libfprint" -Np1 -i "${REPO_DIR}/patches/0001-upstream-libfprint-adst2009.patch"
fi

for _f in adst2009.c adst2009-proto.h; do
  install -Dm644 "${REPO_DIR}/drivers/adst2009/${_f}" \
    "${WORK_DIR}/libfprint/libfprint/drivers/adst2009/${_f}"
done

GIR_OPT="-Dintrospection=false"
if command -v g-ir-scanner >/dev/null 2>&1; then
  GIR_OPT="-Dintrospection=true"
fi

meson setup "${WORK_DIR}/build" "${WORK_DIR}/libfprint" \
  --prefix="${PREFIX}" \
  --libdir="${LIBDIR}" \
  --buildtype=release \
  "${GIR_OPT}" \
  -Dudev_hwdb=enabled \
  -Dinstalled-tests=false \
  -Ddoc=false \
  -Dgtk-examples=false

meson compile -C "${WORK_DIR}/build"
meson test -C "${WORK_DIR}/build" --suite unit-tests --print-errorlogs

"${WORK_DIR}/build/libfprint/fprint-list-supported-devices" | grep -q "2009:7638"

DRV_DIR="${REPO_DIR}/drivers/adst2009"
cc -O2 -Wall -Wextra -I"${DRV_DIR}" \
  "${REPO_DIR}/tests/adst2009-unit-test.c" \
  -o "${WORK_DIR}/adst2009-unit-test"
"${WORK_DIR}/adst2009-unit-test"

# shellcheck disable=SC2046
cc -O2 -Wall -Wextra -I"${DRV_DIR}" \
  "${REPO_DIR}/tools/adst2009-probe.c" \
  -o "${WORK_DIR}/adst2009-probe" \
  $(pkg-config --cflags --libs libusb-1.0)

echo "==> Installing library, udev rules, and helper tools (requires sudo)..."
sudo meson install -C "${WORK_DIR}/build"
sudo install -Dm755 "${WORK_DIR}/adst2009-probe" "${PREFIX}/bin/adst2009-probe"
sudo install -Dm755 "${REPO_DIR}/tools/fprintd-multi" "${PREFIX}/bin/fprintd-multi"
sudo ln -sf fprintd-multi "${PREFIX}/bin/adst-fprintd"
sudo install -Dm644 "${REPO_DIR}/udev/70-adst-2009-libfprint.rules" "/lib/udev/rules.d/70-adst-2009-libfprint.rules" 2>/dev/null \
  || sudo install -Dm644 "${REPO_DIR}/udev/70-adst-2009-libfprint.rules" "/usr/lib/udev/rules.d/70-adst-2009-libfprint.rules"

sudo ldconfig
sudo systemd-hwdb update 2>/dev/null || sudo udevadm hwdb --update 2>/dev/null || true
sudo udevadm control --reload
sudo udevadm trigger --action=change --subsystem-match=usb --attr-match=idVendor=2009
sudo systemctl restart fprintd.service 2>/dev/null || true

echo ""
echo "==> Done! Detected fprintd devices:"
"${PREFIX}/bin/adst-fprintd" status || true
