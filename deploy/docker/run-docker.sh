#!/usr/bin/env bash
# Build + run a QGroundControl Docker builder image for a given variant.
#
# Usage:
#   deploy/docker/run-docker.sh <variant> [build-type]
#     variants:   ubuntu | ubuntu-2204 | ubuntu-2604 | debian | fedora | arch | aarch64 | android
#     build-type: Release (default) | Debug | RelWithDebInfo | MinSizeRel
#
# Env:
#   IMAGE_NAME   Override the image tag (default: per-variant below)
#   BUILD_DIR    Host build output dir (aarch64 default: <repo>/build-aarch64)
#   CLEAN_BUILD  Set 1 to wipe the build dir before configuring (aarch64 only)
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SOURCE_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
DOCKERFILE="${SCRIPT_DIR}/Dockerfile"

usage() {
    echo "Usage: $0 <variant> [build-type]" >&2
    python3 "${SCRIPT_DIR}/_variant_info.py" 2>&1 | sed -n 's/^Valid variants:/  variants:/p' >&2
    exit 1
}

VARIANT="${1:-}"
BUILD_TYPE="${2:-Release}"
[[ -z "${VARIANT}" ]] && usage

# Variant target/image/fuse/build_args come from the shared deploy/docker/variants.json
# (same source CI's plan_docker_builds.py reads), emitted as eval-able bash. Declared
# first so the eval-populated values are visible to shellcheck.
target="" default_image="" fuse="0"
build_args=()
variant_info="$(python3 "${SCRIPT_DIR}/_variant_info.py" "${VARIANT}")" || usage
eval "${variant_info}"

run_flags=()
[[ "${fuse}" == "1" ]] && run_flags=(--fuse)

IMAGE_NAME="${IMAGE_NAME:-${default_image}}"

# Proxy environments (gost HTTP proxy on 127.0.0.1:8118): if http_proxy/https_proxy
# are set, let docker build reach the host proxy and forward the proxy env into the
# build so apt/aqt/uv can fetch Qt etc. Left empty (no behavior change) otherwise.
proxy_build_args=()
if [[ -n "${http_proxy:-}" || -n "${https_proxy:-}" ]]; then
    proxy_build_args=(--network=host --build-arg http_proxy --build-arg https_proxy \
        --build-arg HTTP_PROXY --build-arg HTTPS_PROXY)
fi

docker build \
    --file "${DOCKERFILE}" \
    --target "${target}" \
    ${build_args[@]+"${build_args[@]}"} \
    ${proxy_build_args[@]+"${proxy_build_args[@]}"} \
    -t "${IMAGE_NAME}" \
    "${SOURCE_DIR}"

run_env=(SOURCE_DIR="${SOURCE_DIR}")
if [[ "${VARIANT}" == "aarch64" ]]; then
    BUILD_DIR="${BUILD_DIR:-${SOURCE_DIR}/build-aarch64}"
    # Throttle the local cross-build to half the cores by default (CI sets no JOBS
    # and so runs --parallel); override with JOBS=N.
    run_env+=(BUILD_DIR="${BUILD_DIR}" CLEAN_BUILD="${CLEAN_BUILD:-0}" \
        JOBS="${JOBS:-$(( $(nproc) > 1 ? $(nproc) / 2 : 1 ))}")
fi

env "${run_env[@]}" \
    "${SCRIPT_DIR}/_docker-exec.sh" ${run_flags[@]+"${run_flags[@]}"} "${IMAGE_NAME}" "${BUILD_TYPE}"

if [[ "${VARIANT}" == "aarch64" ]]; then
    echo "Output: ${BUILD_DIR}"
fi
