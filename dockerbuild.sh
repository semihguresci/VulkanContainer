#!/usr/bin/env sh
set -eu

IMAGE_NAME="${IMAGE_NAME:-vulkan-scene-renderer:latest}"

docker build --pull --rm -f dockerfile -t "${IMAGE_NAME}" .

if [ "${RUN_CONTAINER:-0}" = "1" ]; then
    docker run --rm --gpus all "${IMAGE_NAME}"
fi
