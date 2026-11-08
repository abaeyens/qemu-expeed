# syntax=docker/dockerfile:1

# Version pins
ARG QEMU_VERSION=9.1.0

# ---------------------------------------------------------------------------------------------
FROM ubuntu:24.04 AS toolchain

# Keep downloaded .debs, so the /var/cache/apt cache mount below has something to keep
RUN rm -f /etc/apt/apt.conf.d/docker-clean && \
    echo 'Binary::apt::APT::Keep-Downloaded-Packages "true";' > /etc/apt/apt.conf.d/keep-cache

RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    --mount=type=cache,target=/var/lib/apt,sharing=locked \
    apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    build-essential \
    ca-certificates \
    curl \
    python3 \
    xz-utils

# ---------------------------------------------------------------------------------------------
# QEMU's build libraries: the qemu stage compiles against them, and the final image
# needs them as well, because qemu/apply_and_build.sh rebuilds the tree there
FROM toolchain AS qemu-deps
RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    --mount=type=cache,target=/var/lib/apt,sharing=locked \
    apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    # libjpeg-turbo8-dev: the JPEG codec model encodes with it (QEMU's own `jpeg` meson dependency)
    # libgtk-3-dev: the display window (`-display gtk`) on the X11 display docker-compose forwards
    libfdt-dev \
    libglib2.0-dev \
    libgtk-3-dev \
    libjpeg-turbo8-dev \
    libpixman-1-dev \
    meson \
    ninja-build \
    python3-venv \
    zlib1g-dev

# ---------------------------------------------------------------------------------------------
FROM qemu-deps AS qemu
ARG QEMU_VERSION
WORKDIR /opt
RUN <<'EOF'
set -e
curl -fsSLO "https://download.qemu.org/qemu-${QEMU_VERSION}.tar.xz"
tar xf "qemu-${QEMU_VERSION}.tar.xz"
rm "qemu-${QEMU_VERSION}.tar.xz"
cd "qemu-${QEMU_VERSION}"
./configure --target-list=arm-softmmu --disable-docs --disable-werror --enable-gtk --enable-vnc
make -j"$(nproc)"
build/qemu-system-arm -display help | grep -qx gtk
EOF

# ---------------------------------------------------------------------------------------------
FROM qemu-deps AS final

RUN --mount=type=cache,target=/var/cache/apt,sharing=locked \
    --mount=type=cache,target=/var/lib/apt,sharing=locked \
    apt-get update && \
    DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    # --- drive_poweron.py drives the machine through QEMU's gdb stub ---
    gdb-multiarch \
    # --- the tooling Python dependencies ---
    python3-numpy \
    python3-pip \
    # --- build programs/doom, bare-metal ARM with newlib ---
    gcc-arm-none-eabi \
    libnewlib-arm-none-eabi \
    # --- the card image: mkfs.vfat formats it, mtools copies files in without a loop mount ---
    dosfstools \
    mtools \
    # --- the sensor model's exposure from the NEF's EXIF ---
    libimage-exiftool-perl \
    # --- the C sources' autoformatter (.clang-format: 115 columns) ---
    clang-format \
    # --- housekeeping ---
    git \
    less \
    nano \
    sudo \
    vim

# rawpy (LibRaw) reads the NEF the sensor model shows, compressed or not;
# --no-deps so pip leaves the distribution's numpy alone
RUN --mount=type=cache,target=/root/.cache/pip \
    python3 -m pip install --break-system-packages --no-deps "rawpy==0.27.1" && \
    python3 -c "import numpy, rawpy"

# The firmware unpacker tables, from Simeon Pilgrim's Firmware.cs
# https://github.com/simeonpilgrim/nikon-firmware-tools
RUN <<'EOF'
set -e
mkdir -p /opt/nikon-firmware-tools
curl -fsSL -o /opt/nikon-firmware-tools/Firmware.cs \
    "https://raw.githubusercontent.com/simeonpilgrim/nikon-firmware-tools/de8019ee30db7fe7bc61e520da289078ae82dcaa/firmware%20decode/firmware%20decode/Firmware.cs"
EOF

# Own the QEMU tree because we patch and rebuild it in place
ARG QEMU_VERSION
ARG USER_ID
ARG GROUP_ID
COPY --link --from=qemu --chown=${USER_ID}:${GROUP_ID} \
    /opt/qemu-${QEMU_VERSION} /opt/qemu-${QEMU_VERSION}

# The non-root user, at the host's uid/gid
# remove default Ubuntu user first
ARG USER
RUN touch /var/mail/ubuntu && chown ubuntu /var/mail/ubuntu && userdel -r ubuntu && \
    groupadd --gid $GROUP_ID $USER && \
    useradd --uid $USER_ID --gid $GROUP_ID --create-home --shell /bin/bash $USER && \
    echo "$USER ALL=(ALL) NOPASSWD:ALL" >> /etc/sudoers.d/$USER && \
    echo 'PS1="(container) ${debian_chroot:+($debian_chroot)}\[\033[01;32m\]\u@\h\[\033[00m\]:\[\033[01;34m\]\w\[\033[00m\]\$ "' >> /home/$USER/.bashrc

USER $USER

CMD ["/bin/bash"]
