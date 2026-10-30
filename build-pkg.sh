#!/usr/bin/env bash
# Builds the unified 4-family Arch package (libfprint-cs9711-zk9500-betterlife-adst-git)
# inside /tmp so the working tree stays clean. Automatically clones zk9500-libfprint
# and betterlife-347d-libfprint if sibling checkouts are not present locally.
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
WORK_DIR="${ADST_BUILDDIR:-/tmp/pkgbuild-adst}"
ZK9500_DIR="${ZK9500_DIR:-${PROJECT_DIR}/../zk9500-libfprint}"
BETTERLIFE_DIR="${BETTERLIFE_DIR:-${PROJECT_DIR}/../betterlife-347d-libfprint}"

echo "==> Preparing clean build directory in ${WORK_DIR}..."
mkdir -p "${WORK_DIR}"
rm -f "${WORK_DIR}"/libfprint-cs9711-zk9500-betterlife-adst-git-*.pkg.tar.*

if [[ ! -f "${ZK9500_DIR}/drivers/zk9500.c" ]]; then
  ZK9500_DIR="${WORK_DIR}/zk9500-libfprint-src"
  if [[ ! -d "${ZK9500_DIR}/.git" ]]; then
    git clone --depth 1 https://github.com/arqueon/zk9500-libfprint.git "${ZK9500_DIR}"
  fi
fi

if [[ ! -f "${BETTERLIFE_DIR}/drivers/betterlife347d/betterlife347d_moc.c" ]]; then
  BETTERLIFE_DIR="${WORK_DIR}/betterlife-347d-libfprint-src"
  if [[ ! -d "${BETTERLIFE_DIR}/.git" ]]; then
    git clone --depth 1 https://github.com/arqueon/betterlife-347d-libfprint.git "${BETTERLIFE_DIR}"
  fi
fi

install -m644 "${PROJECT_DIR}/PKGBUILD" "${WORK_DIR}/PKGBUILD"
for _f in adst2009.c adst2009-proto.h; do
  install -m644 "${PROJECT_DIR}/drivers/adst2009/${_f}" "${WORK_DIR}/${_f}"
done
install -m644 "${PROJECT_DIR}/tools/adst2009-probe.c" "${WORK_DIR}/adst2009-probe.c"
install -m644 "${PROJECT_DIR}/tests/adst2009-unit-test.c" "${WORK_DIR}/adst2009-unit-test.c"

for _f in betterlife347d_moc.c betterlife347d.c betterlife347d-proto.c betterlife347d-proto.h sm3.c sm3.h sm4.c sm4.h; do
  install -m644 "${BETTERLIFE_DIR}/drivers/betterlife347d/${_f}" "${WORK_DIR}/${_f}"
done
install -m644 "${BETTERLIFE_DIR}/tools/betterlife347d-probe.c" "${WORK_DIR}/betterlife347d-probe.c"

install -m644 "${ZK9500_DIR}/drivers/zk9500.c" "${WORK_DIR}/zk9500.c"
install -m644 "${ZK9500_DIR}/tools/zk9500-probe.c" "${WORK_DIR}/zk9500-probe.c"

install -m755 "${PROJECT_DIR}/tools/fprintd-multi" "${WORK_DIR}/fprintd-multi"
install -m644 "${PROJECT_DIR}/patches/0001-unified-cs9711-zk9500-betterlife347d-adst2009.patch" \
  "${WORK_DIR}/0001-unified-cs9711-zk9500-betterlife347d-adst2009.patch"
install -m644 "${PROJECT_DIR}/udev/70-fprint-multi-readers.rules" \
  "${WORK_DIR}/70-fprint-multi-readers.rules"

cd "${WORK_DIR}"
echo "==> Running makepkg -sfc..."
makepkg -sfc --noconfirm

PKG_FILE="$(ls -1t "${WORK_DIR}"/libfprint-cs9711-zk9500-betterlife-adst-git-[0-9]*.pkg.tar.* | grep -v '\-debug-' | head -n1)"
echo ""
echo "==> Unified 4-reader package built successfully:"
ls -lh "${PKG_FILE}"
