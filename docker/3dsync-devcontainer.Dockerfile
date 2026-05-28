FROM devkitpro/devkitarm:latest AS tools-builder

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

FROM devkitpro/devkitarm:latest

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
    && git lfs install \
    && rm -rf /var/lib/apt/lists/*

COPY --from=tools-builder /usr/local/bin/bannertool /usr/local/bin/bannertool
COPY --from=tools-builder /usr/local/bin/makerom /usr/local/bin/makerom
