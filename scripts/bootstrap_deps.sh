#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
THIRD_PARTY_DIR="${ROOT_DIR}/third_party"
NATS_SRC_DIR="${THIRD_PARTY_DIR}/nats/src"
NATS_INSTALL_DIR="${THIRD_PARTY_DIR}/nats/install"

NATS_REPO="${NATS_REPO:-https://github.com/nats-io/nats.c.git}"
NATS_TAG="${NATS_TAG:-v3.11.0}"

usage() {
  cat <<EOF2
Usage: scripts/bootstrap_deps.sh

Instala nats.c en:
  ${NATS_INSTALL_DIR}

Variables opcionales:
  NATS_REPO  (default: https://github.com/nats-io/nats.c.git)
  NATS_TAG   (default: v3.11.0)
EOF2
}

if [ "${1:-}" = "-h" ] || [ "${1:-}" = "--help" ]; then
  usage
  exit 0
fi

mkdir -p "${THIRD_PARTY_DIR}"
echo "[deps] Bootstrapping nats.c into ${THIRD_PARTY_DIR}"

if [ ! -d "${NATS_SRC_DIR}/.git" ]; then
  mkdir -p "$(dirname "${NATS_SRC_DIR}")"
  git clone --depth 1 --branch "${NATS_TAG}" "${NATS_REPO}" "${NATS_SRC_DIR}"
else
  if git -C "${NATS_SRC_DIR}" fetch --depth 1 origin "${NATS_TAG}"; then
    git -C "${NATS_SRC_DIR}" checkout "${NATS_TAG}"
  else
    echo "[deps] warning: cannot fetch nats.c (${NATS_TAG}); using local checkout" >&2
  fi
fi

cmake -S "${NATS_SRC_DIR}" -B "${NATS_SRC_DIR}/build" \
  -DCMAKE_BUILD_TYPE=Release \
  -DNATS_BUILD_STREAMING=OFF \
  -DNATS_BUILD_EXAMPLES=OFF \
  -DNATS_BUILD_LIB_SHARED=ON \
  -DNATS_BUILD_LIB_STATIC=ON \
  -DNATS_BUILD_WITH_TLS=OFF \
  -DNATS_BUILD_WITH_JWT=OFF \
  -DNATS_BUILD_WITH_WEBSOCKETS=OFF \
  -DCMAKE_INSTALL_PREFIX="${NATS_INSTALL_DIR}"
cmake --build "${NATS_SRC_DIR}/build" -j
cmake --install "${NATS_SRC_DIR}/build"

cat <<EOF2
[deps] Done.
[deps] Local NATS prefix: ${NATS_INSTALL_DIR}
EOF2
