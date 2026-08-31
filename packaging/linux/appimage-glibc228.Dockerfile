FROM quay.io/rockylinux/rockylinux@sha256:e8a49c5403b687db05d4d67333fa45808fbe74f36e683cec7abb1f7d0f2338c6

# Rocky Linux 8 supplies glibc 2.28. GCC Toolset provides modern C++20 support
# without raising that libc floor; libstdc++ and libgcc are bundled later by
# package_appimage.sh.
RUN dnf -y --setopt=install_weak_deps=False install epel-release dnf-plugins-core \
    && dnf config-manager --set-enabled powertools \
    && dnf -y --setopt=install_weak_deps=False install \
        binutils \
        cmake \
        file \
        findutils \
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
        xcb-util-image \
        xcb-util-keysyms \
        xcb-util-renderutil \
        xcb-util-wm \
    && dnf clean all \
    && rm -rf /var/cache/dnf

ENV PATH="/opt/rh/gcc-toolset-13/root/usr/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
ENV CC="/opt/rh/gcc-toolset-13/root/usr/bin/gcc"
ENV CXX="/opt/rh/gcc-toolset-13/root/usr/bin/g++"
