FROM nvidia/cuda:12.6.2-cudnn-devel-ubuntu22.04

ENV DEBIAN_FRONTEND=noninteractive
ENV PROJECT_PATH /usr/src/VulkanSceneRenderer
ENV VCPKG_ROOT=/opt/vcpkg
ENV XDG_RUNTIME_DIR=/tmp/runtime
ENV DISPLAY=:99
ENV LANG=C.UTF-8
ENV LC_ALL=C.UTF-8

RUN mkdir -p /tmp/runtime && chmod 700 /tmp/runtime
RUN sed -i 's|http://archive.ubuntu.com/ubuntu/|http://mirrors.kernel.org/ubuntu/|g' /etc/apt/sources.list

RUN apt-get update && \
    apt-get upgrade -y && \
    apt-get install -y --no-install-recommends \
    xorg-dev \
    libgl1-mesa-dev \
    pkg-config \
    vulkan-tools \
    libvulkan1 \
    libvulkan-dev \
    mesa-vulkan-drivers \
    cmake \
    ninja-build \
    clang \
    git \
    curl \
    zip \
    unzip \
    tar \
    python3 \
    wayland-protocols \
    libwayland-dev \
    libxkbcommon-dev \
    xvfb \
    x11vnc \
    fluxbox \
    vulkan-validationlayers \
    && rm -rf /var/lib/apt/lists/*



WORKDIR ${PROJECT_PATH}
RUN git clone https://github.com/microsoft/vcpkg.git "${VCPKG_ROOT}" \
    && git -C "${VCPKG_ROOT}" checkout 5f8c424e267b7360d451df406eeefb3767985b17 \
    && "${VCPKG_ROOT}/bootstrap-vcpkg.sh"
COPY . ${PROJECT_PATH}
RUN mkdir -p build \
    && cmake -S ${PROJECT_PATH} -B ${PROJECT_PATH}/build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE=${VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake \
        -DENABLE_TESTS=1 \
        -DENABLE_WINDOWED_TESTS=0 \
    && cmake --build ${PROJECT_PATH}/build

CMD Xvfb :99 -screen 0 1024x768x16 & \
    export DISPLAY=:99 && \
    ctest --output-on-failure --test-dir ${PROJECT_PATH}/build
