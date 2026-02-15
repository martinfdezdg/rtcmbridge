#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -lt 2 ]; then
  echo "Usage: $0 <input.rtcm3> <output.solution> [workdir]" >&2
  exit 1
fi

RTCM_IN="$1"
SOL_OUT="$2"
WORKDIR="${3:-${PPP_WORKDIR:-$(mktemp -d)}}"
mkdir -p "$WORKDIR"

CONVBIN_BIN="${CONVBIN_BIN:-convbin}"
RNX2RTKP_BIN="${RNX2RTKP_BIN:-rnx2rtkp}"
PPP_CONF="${PPP_CONF:-scripts/ppp-static.conf}"

# Optionally provide precise products.
# Direct files:
#   PPP_SP3_FILE, PPP_CLK_FILE, PPP_BIA_FILE, PPP_DCB_FILE, PPP_ERP_FILE, PPP_IONEX_FILE
# Or a directory with mixed files:
#   PPP_PRODUCTS_DIR
EXTRA_FILES=()
for f in \
  "${PPP_SP3_FILE:-}" \
  "${PPP_CLK_FILE:-}" \
  "${PPP_BIA_FILE:-}" \
  "${PPP_DCB_FILE:-}" \
  "${PPP_ERP_FILE:-}" \
  "${PPP_IONEX_FILE:-}"; do
  if [ -n "$f" ] && [ -f "$f" ]; then
    EXTRA_FILES+=("$f")
  fi
done

if [ -n "${PPP_PRODUCTS_DIR:-}" ] && [ -d "${PPP_PRODUCTS_DIR}" ]; then
  while IFS= read -r -d '' f; do
    EXTRA_FILES+=("$f")
  done < <(find "${PPP_PRODUCTS_DIR}" -maxdepth 1 -type f \( \
      -name '*.sp3' -o -name '*.SP3' -o \
      -name '*.clk' -o -name '*.CLK' -o \
      -name '*.bia' -o -name '*.BIA' -o \
      -name '*.dcb' -o -name '*.DCB' -o \
      -name '*.erp' -o -name '*.ERP' -o \
      -name '*.ion' -o -name '*.ION' -o \
      -name '*.i' \) -print0)
fi

OBS_FILE="$WORKDIR/ppp.obs"
NAV_FILE="$WORKDIR/ppp.nav"
RTK_SOL="$WORKDIR/ppp_rtk.sol"

"$CONVBIN_BIN" -r rtcm3 -v 3.04 -o "$OBS_FILE" -n "$NAV_FILE" "$RTCM_IN"
"$RNX2RTKP_BIN" -k "$PPP_CONF" -o "$RTK_SOL" "$OBS_FILE" "$NAV_FILE" "${EXTRA_FILES[@]}"

# Expected line format from rnx2rtkp (LLH): date time lat lon h Q ...
LAST_LINE=$(grep -v '^%' "$RTK_SOL" | tail -n 1 || true)
if [ -z "$LAST_LINE" ]; then
  echo "No PPP solution lines found" >&2
  exit 2
fi

LAT=$(echo "$LAST_LINE" | awk '{print $3}')
LON=$(echo "$LAST_LINE" | awk '{print $4}')
HGT=$(echo "$LAST_LINE" | awk '{print $5}')
Q=$(echo "$LAST_LINE" | awk '{print $6}')

# Conservative sigma mapping by quality indicator.
SIGMA=1.5000
case "$Q" in
  1) SIGMA=0.0500 ;; # fixed/very good
  2) SIGMA=0.1200 ;;
  5) SIGMA=0.3000 ;;
  *) SIGMA=1.5000 ;;
esac

python3 - "$LAT" "$LON" "$HGT" "$SIGMA" <<'PY' > "$SOL_OUT"
import math
import sys

lat = math.radians(float(sys.argv[1]))
lon = math.radians(float(sys.argv[2]))
h = float(sys.argv[3])
sigma = float(sys.argv[4])

a = 6378137.0
f = 1.0 / 298.257223563
e2 = f * (2.0 - f)
N = a / math.sqrt(1.0 - e2 * math.sin(lat) ** 2)
x = (N + h) * math.cos(lat) * math.cos(lon)
y = (N + h) * math.cos(lat) * math.sin(lon)
z = (N * (1.0 - e2) + h) * math.sin(lat)
print(f"{x:.4f} {y:.4f} {z:.4f} {sigma:.4f}")
PY
