#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
THIRD_PARTY_DIR="${ROOT_DIR}/third_party"
NATS_SRC_DIR="${THIRD_PARTY_DIR}/nats/src"
NATS_INSTALL_DIR="${THIRD_PARTY_DIR}/nats/install"
RTKLIB_SRC_DIR="${THIRD_PARTY_DIR}/rtklib/src"
RTKLIB_BIN_DIR="${THIRD_PARTY_DIR}/rtklib/bin"

NATS_REPO="${NATS_REPO:-https://github.com/nats-io/nats.c.git}"
NATS_TAG="${NATS_TAG:-v3.11.0}"
# demo5 fork by default (better MSM/Galileo/BeiDou support).
RTKLIB_REPO="${RTKLIB_REPO:-https://github.com/rtklibexplorer/RTKLIB.git}"
RTKLIB_TAG="${RTKLIB_TAG:-demo5}"

INSTALL_NATS=1
INSTALL_RTKLIB=1
UNAME_S="$(uname -s || true)"

usage() {
  cat <<EOF
Usage: scripts/bootstrap_deps.sh [--rtklib-only] [--nats-only] [--rtklib-upstream] [--rtklib-demo5]

Options:
  --rtklib-only   Install only RTKLIB tools (convbin, rnx2rtkp).
  --nats-only     Install only nats.c library.
  --rtklib-upstream  Use official RTKLIB upstream (tomojitakasu).
  --rtklib-demo5     Use demo5 RTKLIB fork (rtklibexplorer). Default.
  -h, --help      Show this help.
EOF
}

for arg in "$@"; do
  case "$arg" in
    --rtklib-only)
      INSTALL_NATS=0
      INSTALL_RTKLIB=1
      ;;
    --nats-only)
      INSTALL_NATS=1
      INSTALL_RTKLIB=0
      ;;
    --rtklib-upstream)
      RTKLIB_REPO="https://github.com/tomojitakasu/RTKLIB.git"
      if [ "${RTKLIB_TAG}" = "demo5" ]; then
        RTKLIB_TAG="master"
      fi
      ;;
    --rtklib-demo5)
      RTKLIB_REPO="https://github.com/rtklibexplorer/RTKLIB.git"
      if [ "${RTKLIB_TAG}" = "master" ]; then
        RTKLIB_TAG="demo5"
      fi
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "[deps] unknown option: $arg" >&2
      usage
      exit 1
      ;;
  esac
done

mkdir -p "${THIRD_PARTY_DIR}"
echo "[deps] Bootstrapping dependencies into ${THIRD_PARTY_DIR}"

if [ "${INSTALL_NATS}" -eq 1 ]; then
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
fi

if [ "${INSTALL_RTKLIB}" -eq 1 ]; then
  echo "[deps] RTKLIB source: ${RTKLIB_REPO} (tag/branch: ${RTKLIB_TAG})"
  if [ ! -d "${RTKLIB_SRC_DIR}/.git" ]; then
    mkdir -p "$(dirname "${RTKLIB_SRC_DIR}")"
    if git clone --depth 1 --branch "${RTKLIB_TAG}" "${RTKLIB_REPO}" "${RTKLIB_SRC_DIR}"; then
      :
    elif git clone --depth 1 --branch "main" "${RTKLIB_REPO}" "${RTKLIB_SRC_DIR}"; then
      echo "[deps] warning: branch ${RTKLIB_TAG} not found; using main" >&2
    else
      git clone --depth 1 "${RTKLIB_REPO}" "${RTKLIB_SRC_DIR}"
      echo "[deps] warning: branch ${RTKLIB_TAG} not found; using repository default branch" >&2
    fi
  else
    CURRENT_ORIGIN="$(git -C "${RTKLIB_SRC_DIR}" remote get-url origin 2>/dev/null || true)"
    if [ "${CURRENT_ORIGIN}" != "${RTKLIB_REPO}" ]; then
      git -C "${RTKLIB_SRC_DIR}" remote set-url origin "${RTKLIB_REPO}"
    fi
    if git -C "${RTKLIB_SRC_DIR}" fetch --depth 1 origin "${RTKLIB_TAG}"; then
      git -C "${RTKLIB_SRC_DIR}" checkout "${RTKLIB_TAG}"
    elif git -C "${RTKLIB_SRC_DIR}" fetch --depth 1 origin main; then
      git -C "${RTKLIB_SRC_DIR}" checkout main
      echo "[deps] warning: cannot fetch ${RTKLIB_TAG}; using main" >&2
    else
      echo "[deps] warning: cannot fetch RTKLIB (${RTKLIB_TAG}); using local checkout" >&2
    fi
  fi

  mkdir -p "${RTKLIB_BIN_DIR}"
  if [ -d "${RTKLIB_SRC_DIR}/app/consapp/convbin/gcc" ]; then
    CONVBIN_DIR="${RTKLIB_SRC_DIR}/app/consapp/convbin/gcc"
  elif [ -d "${RTKLIB_SRC_DIR}/app/convbin/gcc" ]; then
    CONVBIN_DIR="${RTKLIB_SRC_DIR}/app/convbin/gcc"
  else
    echo "[deps] convbin build dir not found in RTKLIB tree" >&2
    exit 1
  fi

  if [ -d "${RTKLIB_SRC_DIR}/app/consapp/rnx2rtkp/gcc" ]; then
    RNX2RTKP_DIR="${RTKLIB_SRC_DIR}/app/consapp/rnx2rtkp/gcc"
  elif [ -d "${RTKLIB_SRC_DIR}/app/rnx2rtkp/gcc" ]; then
    RNX2RTKP_DIR="${RTKLIB_SRC_DIR}/app/rnx2rtkp/gcc"
  else
    echo "[deps] rnx2rtkp build dir not found in RTKLIB tree" >&2
    exit 1
  fi

  if [ "${UNAME_S}" = "Darwin" ]; then
    RTKLIB_CC='cc -D_DARWIN_C_SOURCE -D_POSIX_C_SOURCE=200809L'
    RTKLIB_LDLIBS='-lm'
  else
    RTKLIB_CC='cc'
    RTKLIB_LDLIBS='-lm -lrt'
  fi

  make -C "${CONVBIN_DIR}" clean all CC="${RTKLIB_CC}" LDLIBS="${RTKLIB_LDLIBS}"
  cp "${CONVBIN_DIR}/convbin" "${RTKLIB_BIN_DIR}/convbin"

  make -C "${RNX2RTKP_DIR}" clean all CC="${RTKLIB_CC}" LDLIBS="${RTKLIB_LDLIBS}"
  cp "${RNX2RTKP_DIR}/rnx2rtkp" "${RTKLIB_BIN_DIR}/rnx2rtkp"

  chmod +x "${RTKLIB_BIN_DIR}/convbin" "${RTKLIB_BIN_DIR}/rnx2rtkp"
fi

cat <<EOF
[deps] Done.
[deps] Local NATS prefix: ${NATS_INSTALL_DIR}
[deps] Local RTKLIB bin:  ${RTKLIB_BIN_DIR}
EOF
