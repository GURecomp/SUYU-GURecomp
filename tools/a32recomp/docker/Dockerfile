# Linux build environment for suyu (A32 recompiler): Ubuntu 22.04 (glibc 2.35, the oldest base
# the Linux release supports), gcc 13, CMake, Ninja, the X11/Wayland/audio headers and Qt 6.
# Qt license texts (aqt installs none) come from the matching qtbase tag, for the package.
# No game data, keys or suyu binaries: the source is mounted at run time.
#
#   docker build -t suyu-a32-build tools/a32recomp/docker
#   docker run --rm -it -v <source>:/opt/suyu-src -w /opt/suyu-src suyu-a32-build \
#     tools/a32recomp/build_release_package.sh 0.2.0 <forbidden words>
FROM ubuntu:22.04

ARG DEBIAN_FRONTEND=noninteractive
ARG QT_VERSION=6.8.3

RUN apt-get update && apt-get install -y --no-install-recommends software-properties-common gnupg gpg-agent \
 && add-apt-repository -y ppa:ubuntu-toolchain-r/test \
 && apt-get update && apt-get install -y --no-install-recommends \
      gcc-13 g++-13 git ninja-build python3 python3-pip pkg-config glslang-tools ccache \
      curl zip unzip tar xz-utils file make patch patchelf nasm autoconf automake libtool \
      libgl-dev libegl-dev libx11-dev libxext-dev libxrandr-dev libxi-dev libxcursor-dev \
      libxinerama-dev libxss-dev libxkbcommon-dev libxkbcommon-x11-dev libxcb1-dev \
      libxcb-cursor0 libxcb-icccm4 libxcb-image0 libxcb-keysyms1 libxcb-randr0 \
      libxcb-render-util0 libxcb-shape0 libxcb-xinerama0 libxcb-xkb1 libwayland-dev \
      libasound2-dev libpulse-dev libjack-dev libpipewire-0.3-dev libudev-dev libdbus-1-dev \
      libfontconfig1-dev libfreetype-dev libssl-dev libusb-1.0-0-dev libxtst-dev libxfixes-dev \
      libxrender-dev libdrm-dev libgbm-dev libgles2-mesa-dev libdecor-0-dev libsndio-dev \
      libva-dev libvdpau-dev libvulkan-dev \
      mesa-vulkan-drivers vulkan-tools xvfb \
 && update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-13 100 \
      --slave /usr/bin/g++ g++ /usr/bin/g++-13 \
 && update-alternatives --install /usr/bin/cc cc /usr/bin/gcc 100 \
 && update-alternatives --install /usr/bin/c++ c++ /usr/bin/g++ 100 \
 && rm -rf /var/lib/apt/lists/*

# Ubuntu 22.04's CMake is 3.22 (suyu's minimum); a newer one from PyPI avoids FetchContent quirks.
RUN python3 -m pip install --no-cache-dir "cmake>=3.28,<4" aqtinstall \
 && aqt install-qt linux desktop ${QT_VERSION} linux_gcc_64 -O /opt/Qt \
      -m qtcharts qtimageformats qtmultimedia qtwaylandcompositor \
 && rm -rf /root/.cache aqtinstall.log \
 && git clone -q --depth 1 --filter=blob:none --sparse -b v${QT_VERSION} \
      https://github.com/qt/qtbase.git /tmp/qtbase \
 && git -C /tmp/qtbase sparse-checkout set LICENSES \
 && cp -r /tmp/qtbase/LICENSES /opt/Qt/Licenses && rm -rf /tmp/qtbase

ENV QT_DIR=/opt/Qt/${QT_VERSION}/gcc_64 \
    CMAKE_PREFIX_PATH=/opt/Qt/${QT_VERSION}/gcc_64 \
    QT_LICENSES=/opt/Qt/Licenses
