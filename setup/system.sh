#!/usr/bin/env bash
# The system packages the environment needs, on $BASE_IMAGE (Ubuntu 22.04). Needs root.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=versions.env
source "$HERE/versions.env"

export DEBIAN_FRONTEND=noninteractive

apt-get update
apt-get install -y --no-install-recommends software-properties-common ca-certificates gnupg
add-apt-repository -y ppa:deadsnakes/ppa
apt-get update

apt-get install -y --no-install-recommends \
  build-essential git curl m4 pkg-config \
  cmake ninja-build \
  "$PYTHON" "$PYTHON-dev" "$PYTHON-venv" \
  zlib1g-dev libprotobuf-dev protobuf-compiler libgoogle-perftools-dev \
  libhdf5-serial-dev libpng-dev \
  libboost-dev libboost-regex-dev libboost-system-dev device-tree-compiler \
  python3 autoconf flex bison libfl-dev help2man
rm -rf /var/lib/apt/lists/*

SITE="$("$PYTHON" -c 'import site; print(site.getsitepackages()[0])')"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
"$PYTHON" -m venv "$TMP/venv"
"$TMP/venv/bin/pip" install -q --no-cache-dir --upgrade --target "$SITE" "PyYAML==$PYYAML_VERSION"
"$PYTHON" -c 'import yaml; print("PyYAML", yaml.__version__, "for", yaml.__file__)'
