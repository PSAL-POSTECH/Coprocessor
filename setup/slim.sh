#!/usr/bin/env bash
# Cut what setup.sh built down to what is used, for the image only.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=versions.env
source "$HERE/versions.env"

# Of Spike the installed binary is used, and riscv/coprocessor.h of its source, which the tests compare with ours.
rm -rf "$SPIKE_BUILD" "$SPIKE_ROOT/.git" "$PK_ROOT" "$VERILATOR_SRC"

for tool in clang-check clang-repl clang-refactor clang-rename clang-scan-deps \
            clang-extdef-mapping clang-linker-wrapper clang-offload-bundler \
            clang-offload-packager diagtool qemu-riscv32 qemu-riscv64; do
  rm -f "$TOOLCHAIN_ROOT/bin/$tool"
done

# The ELF e_machine of this host, little-endian: EM_X86_64 (62) or EM_AARCH64 (183).
case "$HOST_ARCH" in
  x86_64) HOST_EM=3e00 ;;
  aarch64) HOST_EM=b700 ;;
esac

# Whether $1 is an ELF of this host.
host_elf() {
  [ "$(od -An -tx1 -N4 "$1" | tr -d ' ')" = 7f454c46 ] \
    && [ "$(od -An -tx1 -j18 -N2 "$1" | tr -d ' ')" = "$HOST_EM" ]
}

strip_tree() {
  local f
  while IFS= read -r -d '' f; do
    if host_elf "$f"; then strip --strip-debug "$f"; fi
  done < <(find "$@" -type f -print0)
}

strip_tree "$SPIKE_PREFIX" "$TOOLCHAIN_ROOT/bin" "$TOOLCHAIN_ROOT/libexec" "$VERILATOR_PREFIX/bin"
strip --strip-debug "$SPIKE_PREFIX"/lib/*.a

du -sh "$COPROCESSOR_ENV_ROOT"/*
