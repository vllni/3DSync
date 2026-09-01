FROM docker.io/devkitpro/devkitarm:latest AS tools-builder

# Build dependencies used only for bannertool and makerom.
RUN apt-get update && apt-get install -y --no-install-recommends \
    cmake \
    build-essential \
    git \
    && rm -rf /var/lib/apt/lists/*

RUN git clone --depth=1 https://github.com/carstene1ns/3ds-bannertool.git /tmp/bannertool \
    && cmake -DCMAKE_BUILD_TYPE=Release -B /tmp/bannertool/build /tmp/bannertool \
    && cmake --build /tmp/bannertool/build -j$(nproc) \
    && install -m755 /tmp/bannertool/build/bannertool /usr/local/bin/bannertool

RUN git clone --depth=1 https://github.com/3DSGuy/Project_CTR.git /tmp/Project_CTR \
    && make -C /tmp/Project_CTR/makerom deps \
    && make -C /tmp/Project_CTR/makerom -j$(nproc) \
    && install -m755 /tmp/Project_CTR/makerom/bin/makerom /usr/local/bin/makerom

# libsmb2 provides the SMB2/SMB3 client for the [SMB] remote.  devkitPro ships
# no package for it, so it is cross-compiled here and dropped into portlibs.
# Pinned to a commit so an image rebuild cannot silently change the library.
FROM docker.io/devkitpro/devkitarm:latest AS smb2-builder

ARG LIBSMB2_REF=3f265f98ac232cbb1396594f13c1b5552a77945c

RUN dkp-pacman -Sy --noconfirm 3ds-dev && dkp-pacman -Scc --noconfirm

RUN apt-get update && apt-get install -y --no-install-recommends \
    cmake \
    build-essential \
    git \
    && rm -rf /var/lib/apt/lists/*

RUN git clone https://github.com/sahlberg/libsmb2.git /tmp/libsmb2 \
    && git -C /tmp/libsmb2 checkout "${LIBSMB2_REF}" \
    && cmake -S /tmp/libsmb2 -B /tmp/libsmb2/build \
        -DCMAKE_TOOLCHAIN_FILE=/opt/devkitpro/cmake/3DS.cmake \
        -DCMAKE_INSTALL_PREFIX=/opt/devkitpro/portlibs/3ds \
        -DBUILD_SHARED_LIBS=OFF \
        -DENABLE_EXAMPLES=OFF \
        -DENABLE_LIBKRB5=OFF \
        -DENABLE_GSSAPI=OFF \
        -DENABLE_LIBDCERPC=OFF \
    && cmake --build /tmp/libsmb2/build -j$(nproc) \
    && cmake --install /tmp/libsmb2/build \
    && install -Dm644 /tmp/libsmb2/COPYING /opt/licenses/libsmb2-COPYING \
    && rm -rf /tmp/libsmb2

FROM docker.io/devkitpro/devkitarm:latest

# Install 3DS development tools and required portlibs
RUN dkp-pacman -Sy --noconfirm \
    3ds-dev \
    3ds-curl \
    3ds-mbedtls \
    3ds-zlib \
    && dkp-pacman -Scc --noconfirm

# Install runtime utilities used by Codespaces and the Makefile.
RUN apt-get update && apt-get install -y --no-install-recommends \
    git \
    git-lfs \
    gh \
    openssh-client \
    && git lfs install \
    && rm -rf /var/lib/apt/lists/*

COPY --from=tools-builder /usr/local/bin/bannertool /usr/local/bin/bannertool
COPY --from=tools-builder /usr/local/bin/makerom /usr/local/bin/makerom

# libsmb2 (LGPL-2.1) — headers, static library and its licence text.
COPY --from=smb2-builder /opt/devkitpro/portlibs/3ds/include/smb2 /opt/devkitpro/portlibs/3ds/include/smb2
COPY --from=smb2-builder /opt/devkitpro/portlibs/3ds/lib/libsmb2.a /opt/devkitpro/portlibs/3ds/lib/libsmb2.a
COPY --from=smb2-builder /opt/licenses/libsmb2-COPYING /opt/licenses/libsmb2-COPYING
