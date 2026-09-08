FROM fedora:40

# Native Linux toolchain + libraries (software CPU and KVM backends), plus the
# MinGW-w64 cross toolchain and Windows builds of libcurl/OpenSSL so the Windows
# .exe can fetch remote configs and assets (CONFIG_FS_NET) and do user-mode
# networking (CONFIG_SLIRP).
RUN dnf -y install \
        gcc \
        make \
        git \
        file \
        findutils \
        diffutils \
        ca-certificates \
        kernel-headers \
        openssl-devel \
        libcurl-devel \
        SDL-devel \
        mingw64-gcc \
        mingw64-winpthreads \
        mingw64-winpthreads-static \
        mingw64-curl \
        mingw64-openssl \
    && dnf clean all

WORKDIR /workspace

COPY . /workspace

RUN chmod +x tests/*.sh scripts/*.sh 2>/dev/null || true

CMD ["bash"]
