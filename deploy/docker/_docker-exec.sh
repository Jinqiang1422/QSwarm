#!/usr/bin/env bash
set -euo pipefail

usage() {
    echo "Usage: $0 [--fuse] <image-name> [build-type]"
    echo "  --fuse    Enable FUSE support (required for AppImage builds)"
    echo ""
    echo "Env:"
    echo "  SOURCE_DIR   Host source tree    (default: \$PWD)"
    echo "  BUILD_DIR    Host build output   (default: \$SOURCE_DIR/build)"
    echo "  CLEAN_BUILD  Set 1 to wipe the build dir before configuring (cross image only)"
    exit 1
}

FUSE_FLAGS=()
while [[ $# -gt 0 && "$1" == --* ]]; do
    case "$1" in
        --fuse)
            FUSE_FLAGS=(--cap-add SYS_ADMIN --device /dev/fuse --security-opt apparmor:unconfined)
            shift
            ;;
        --help|-h)
            usage
            ;;
        *)
            echo "Unknown option: $1" >&2
            usage
            ;;
    esac
done

IMAGE_NAME="${1:-}"
BUILD_TYPE="${2:-Release}"
SOURCE_DIR="${SOURCE_DIR:-$(pwd)}"
BUILD_DIR="${BUILD_DIR:-${SOURCE_DIR}/build}"

[[ -z "${IMAGE_NAME}" ]] && usage

mkdir -p "${BUILD_DIR}"

# --user matches the host UID so on-disk build artifacts aren't root-owned.
# HOME is forced to a writable path because a mapped UID has no /etc/passwd
# entry (gradle/ccache need a writable home). Source is mounted read-write: the
# build writes a compile_commands.json symlink and the CPM cache into the tree.
# Proxy environments (gost HTTP proxy on 127.0.0.1:8118): the cmake configure step
# runs CPM/FetchContent git clone (e.g. geographiclib from github) inside this
# container, so it needs host network + the proxy env forwarded. Empty otherwise.
proxy_run_args=()
if [[ -n "${http_proxy:-}" || -n "${https_proxy:-}" ]]; then
    proxy_run_args=(--network=host \
        --env http_proxy --env https_proxy --env HTTP_PROXY --env HTTPS_PROXY)
fi

docker run \
    --rm \
    --user "$(id -u):$(id -g)" \
    --env HOME=/tmp \
    --env CLEAN_BUILD="${CLEAN_BUILD:-0}" \
    --env JOBS="${JOBS:-}" \
    ${FUSE_FLAGS[@]+"${FUSE_FLAGS[@]}"} \
    ${proxy_run_args[@]+"${proxy_run_args[@]}"} \
    -v "${SOURCE_DIR}:/project/source" \
    -v "${BUILD_DIR}:/project/build" \
    "${IMAGE_NAME}" \
    "${BUILD_TYPE}"
