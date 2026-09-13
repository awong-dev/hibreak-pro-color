# Linux toolbox for HiBreak Pro Color super-image work.
#
# The loopback7084 debloat scripts need GNU coreutils, loop mounts, ext4 and
# root -- none of which macOS provides. Everything that touches a filesystem
# image happens in here; mtkclient/adb/fastboot stay on the host because they
# need USB.
FROM debian:bookworm

RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      build-essential clang git ca-certificates \
      e2fsprogs util-linux mount kmod \
      android-sdk-libsparse-utils \
      squashfs-tools \
      python3 python3-pip \
      file binutils xxd less vim-tiny locate \
      zlib1g-dev \
 && rm -rf /var/lib/apt/lists/*

# lpunpack / lpmake / lpdump / lpadd, built from the AOSP sources
RUN git clone --depth 1 https://github.com/LonelyFool/lpunpack_and_lpmake.git /opt/lp \
 && cd /opt/lp && ./make.sh >/dev/null 2>&1 \
 && install -m0755 /opt/lp/bin/* /usr/local/bin/ \
 && rm -rf /opt/lp \
 && updatedb   # safedebloat's unpack.sh probes for lpunpack with `locate`

WORKDIR /work
CMD ["/bin/bash"]
