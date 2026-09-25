#!/bin/sh
# MVX — a native compiler and runtime for Pick/MultiValue BASIC.
# Copyright (C) 2026 Gordon Heydon.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 2, as
# published by the Free Software Foundation.  There is NO WARRANTY, to
# the extent permitted by law; see the LICENSE file for details.
#
# SPDX-License-Identifier: GPL-2.0-only
#
# Build (and optionally push) the three MVX images: the base system, the
# LMDB daemon, and the demo account.  Run from the repository root.
#
#   docker/build.sh                       # build ghcr.io/mvx-lang/mvx{,-lmdbd,-demo}:latest
#   REGISTRY=you TAG=0.1 docker/build.sh  # custom namespace and tag
#   PUSH=1 docker/build.sh                # build then docker push each
#
set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

REGISTRY="${REGISTRY:-ghcr.io/mvx-lang}"
TAG="${TAG:-latest}"
LLVM_VERSION="${LLVM_VERSION:-21}"
# The image cannot work out its own version -- .dockerignore excludes .git --
# so pass it in, the way the release does (mvx#299).  Unset and undescribable
# leaves the binaries saying 0.0.0-dev, which is at least honest.
MVX_VERSION="${MVX_VERSION:-$(git describe --tags --match 'v*' --dirty 2>/dev/null | sed 's/^v//')}"

BASE="${REGISTRY}/mvx"
LMDBD="${REGISTRY}/mvx-lmdbd"
DEMO="${REGISTRY}/mvx-demo"

echo "==> base  ${BASE}:${TAG}"
docker build -f docker/Dockerfile.base \
  --build-arg "LLVM_VERSION=${LLVM_VERSION}" \
  --build-arg "MVX_VERSION=${MVX_VERSION}" \
  -t "${BASE}:${TAG}" -t mvx:latest .

# The daemon and demo layer on the freshly built base.
echo "==> daemon ${LMDBD}:${TAG}"
docker build -f docker/Dockerfile.lmdbd --build-arg "BASE=mvx:latest" \
  -t "${LMDBD}:${TAG}" .

echo "==> demo   ${DEMO}:${TAG}"
docker build -f docker/Dockerfile.demo --build-arg "BASE=mvx:latest" \
  -t "${DEMO}:${TAG}" .

if [ "${PUSH:-0}" = 1 ]; then
  for img in "${BASE}" "${LMDBD}" "${DEMO}"; do
    echo "==> push ${img}:${TAG}"
    docker push "${img}:${TAG}"
  done
fi

echo "done: ${BASE}:${TAG}  ${LMDBD}:${TAG}  ${DEMO}:${TAG}"
