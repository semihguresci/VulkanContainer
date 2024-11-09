FROM nvidia/cuda:12.6.2-cudnn-devel-ubuntu22.04

ENV DEBIAN_FRONTEND=noninteractive
ENV PROJECT_PATH /usr/src/Container
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
    clang \
    git \
    wayland-protocols \
    libwayland-dev \
    libxkbcommon-dev \
    xvfb \
    x11vnc \
    fluxbox \
    vulkan-validationlayers \
    && rm -rf /var/lib/apt/lists/*



WORKDIR ${PROJECT_PATH}
COPY ../. ${PROJECT_PATH}
RUN mkdir -p build \
    && cmake -S ${PROJECT_PATH} -B ${PROJECT_PATH}/build -DCMAKE_BUILD_TYPE=Release -DENABLE_TESTS=1 \
    && cmake --build ${PROJECT_PATH}/build --target build_tests 

CMD Xvfb :99 -screen 0 1024x768x16 & \
    export DISPLAY=:99 && \
    cmake --build ${PROJECT_PATH}/build --target run_tests && \
    ctest --output-on-failure --test-dir ${PROJECT_PATH}/build
