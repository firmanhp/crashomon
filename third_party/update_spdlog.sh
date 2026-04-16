#!/usr/bin/env bash
# third_party/update_spdlog.sh — refresh vendored spdlog headers
#
# Usage:
#   ./third_party/update_spdlog.sh v1.14.1
#
# Clones spdlog at the given tag into a temp directory, replaces
# third_party/spdlog/include/ with the new headers, and records the
# version in third_party/spdlog/VERSION.
#
# After running, commit the result:
#   git add third_party/spdlog
#   git commit -m "Update vendored spdlog to <version>"

set -euo pipefail

REPO="https://github.com/gabime/spdlog.git"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEST="${SCRIPT_DIR}/spdlog"

if [[ $# -ne 1 ]]; then
  echo "Usage: $0 <git-tag>   e.g. $0 v1.14.1" >&2
  exit 1
fi

TAG="$1"

echo "Fetching spdlog ${TAG} ..."
TMP="$(mktemp -d)"
trap 'rm -rf "${TMP}"' EXIT

git clone --quiet --depth 1 --branch "${TAG}" "${REPO}" "${TMP}/spdlog"

# Verify the include directory exists in the cloned repo
if [[ ! -d "${TMP}/spdlog/include/spdlog" ]]; then
  echo "Error: include/spdlog not found in cloned repo — wrong tag?" >&2
  exit 1
fi

echo "Replacing ${DEST}/include/ ..."
rm -rf "${DEST}/include"
cp -r "${TMP}/spdlog/include" "${DEST}/include"

# Record the pinned version so it's visible without reading git log
echo "${TAG}" > "${DEST}/VERSION"

echo "Done. spdlog vendored headers updated to ${TAG}."
echo "Next steps:"
echo "  git add third_party/spdlog"
echo "  git commit -m \"Update vendored spdlog to ${TAG}\""
