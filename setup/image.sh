#!/usr/bin/env bash
# The container image of the environment: what it is called, and how it is built. One build per host architecture,
# pushed as <ref>-x64 / <ref>-arm64; `join` makes <ref> the manifest list of the two.
# Usage: setup/image.sh tag | ref | exists | arch-exists | build [-j N] [--push] | join
set -euo pipefail
shopt -s inherit_errexit

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(dirname "$HERE")"
# shellcheck source=versions.env
source "$HERE/versions.env"
DOCKER="${DOCKER:-docker}"

image_tag() {
  local tree dirty=""
  tree="$(git -C "$REPO" rev-parse HEAD:setup)"
  [ -z "$(git -C "$REPO" status --porcelain -- setup)" ] || dirty=-dirty
  echo "env-${tree:0:12}$dirty"
}

REF="$IMAGE_REPO:$(image_tag)"
case "$HOST_ARCH" in
  x86_64) ARCH=x64 ;;
  aarch64) ARCH=arm64 ;;
esac

case "${1:-}" in
  tag) echo "${REF##*:}" ;;
  ref) echo "$REF" ;;
  exists) "$DOCKER" buildx imagetools inspect "$REF" > /dev/null ;;
  arch-exists) "$DOCKER" buildx imagetools inspect "$REF-$ARCH" > /dev/null ;;
  build)
    shift
    JOBS=$(nproc); OUTPUT=--load
    while [ $# -gt 0 ]; do
      case "$1" in
        -j) JOBS="$2"; shift 2 ;;
        --push) OUTPUT=--push; shift ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
      esac
    done
    case "$OUTPUT$REF" in
      --push*-dirty) echo "setup/ has uncommitted changes; commit them before publishing" >&2; exit 1 ;;
    esac
    # Pushed under this architecture's tag only; loaded locally under the plain one as well.
    TAGS=(-t "$REF-$ARCH")
    [ "$OUTPUT" = --push ] || TAGS+=(-t "$REF")
    "$DOCKER" buildx build "$OUTPUT" "${TAGS[@]}" -f "$HERE/Dockerfile" \
      --build-arg BASE_IMAGE="$BASE_IMAGE" \
      --build-arg VCIX_ENV_ROOT="$VCIX_ENV_ROOT" \
      --build-arg JOBS="$JOBS" \
      "$HERE"
    echo "$REF-$ARCH"
    ;;
  join)
    "$DOCKER" buildx imagetools create -t "$REF" "$REF-x64" "$REF-arm64"
    got=$("$DOCKER" buildx imagetools inspect --format '{{range .Manifest.Manifests}}{{.Platform.Architecture}} {{end}}' "$REF")
    for want in amd64 arm64; do
      case " $got " in *" $want "*) ;; *) echo "$REF has no $want ($got)" >&2; exit 1 ;; esac
    done
    echo "$REF: $got"
    ;;
  *) sed -n '2,3p' "${BASH_SOURCE[0]}" | sed 's/^# \?//' >&2; exit 2 ;;
esac
