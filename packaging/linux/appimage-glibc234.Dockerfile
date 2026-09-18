FROM quay.io/rockylinux/rockylinux@sha256:8101994123cf3d0a8fee517bee7f39e555c7d92bd2d9eb3303cc988a0eeed00f

# Rocky Linux 9 supplies glibc 2.34. GCC Toolset provides modern C++20 support
# without raising that libc floor; libstdc++ and libgcc are bundled later by
# scripts/package/appimage.sh.
RUN dnf -y --setopt=install_weak_deps=False install epel-release dnf-plugins-core \
    && dnf config-manager --set-enabled crb \
    && dnf -y --setopt=install_weak_deps=False install \
        binutils \
        cmake \
        file \
        findutils \
        fontconfig-devel \
        freetype-devel \
        gcc-toolset-13-gcc-c++ \
        libcurl-devel \
        libsecret-devel \
        libssh2-devel \
        libxkbcommon \
        libxkbcommon-x11 \
        make \
        mesa-libGL-devel \
        ninja-build \
        openssl-devel \
        patchelf \
        pkgconf-pkg-config \
        tinyxml2-devel \
        xcb-util \
        xcb-util-cursor \
        xcb-util-image \
        xcb-util-keysyms \
        xcb-util-renderutil \
        xcb-util-wm \
    && dnf clean all \
    && rm -rf /var/cache/dnf

ENV PATH="/opt/rh/gcc-toolset-13/root/usr/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
ENV CC="/opt/rh/gcc-toolset-13/root/usr/bin/gcc"
ENV CXX="/opt/rh/gcc-toolset-13/root/usr/bin/g++"
