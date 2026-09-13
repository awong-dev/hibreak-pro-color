# Toolchain

Host: macOS (Apple Silicon, arm64). Set up 2026-09-12. See `CLAUDE.md` for the
mission and the GREEN/RED command rules — **this file does not relax any of
them**. Everything below is installed and smoke-tested; nothing has touched the
device.

## Fresh checkout

```bash
tools/bootstrap.sh       # idempotent; installs brew deps, clones, venv, image
source tools/env.sh      # bash or zsh
```

The upstream checkouts (`tools/mtkclient`, `tools/hibreak_pro_color_scripts`,
`tools/otatools-src`), the venv and the built binaries are all gitignored —
`bootstrap.sh` is what reconstitutes them.

Puts `bin/`, `tools/otatools/bin`, Homebrew's keg-only `e2fsprogs`, and GNU
coreutils (ahead of the BSD ones) on `PATH`.

## Where things run

macOS cannot loop-mount ext4, and the loopback7084 debloat scripts need
`mount -o loop`, `resize2fs`, `e2fsck` and GNU `stat -c`. So the work is split:

| | Runs on | Why |
| --- | --- | --- |
| mtkclient, adb, fastboot, scrcpy | macOS host | needs USB |
| lpunpack / lpdump / lpmake | either | native arm64 build **and** in the container |
| mount / resize2fs / e2fsck / debloat | Linux container | macOS has no ext4 |
| binwalk, strings, file, unsquashfs | macOS host | analysis only |

## Host commands

| Command | Notes |
| --- | --- |
| `bin/mtk <args>` | mtkclient wrapper — project venv (python3.11), keeps your cwd so relative output paths work. `bin/mtk printgpt`, `bin/mtk rl --skip=userdata work/backup/out`. **`w`/`e`/`da` are RED.** |
| `bin/hibreak-shell [cmd]` | root shell in the Linux toolbox, `./work` bind-mounted at `/work`, the loopback7084 scripts read-only at `/scripts`. Builds the image on first use. |
| `lpunpack`, `lpdump`, `lpmake`, `lpadd`, `lpflash` | native arm64, in `tools/otatools/bin` |
| `adb`, `fastboot` | platform-tools 37.0.1 |
| `scrcpy` | needed for §7.5 — start it *before* rebooting into a GSI |
| `binwalk` 3.1.0, `unsquashfs`, `debugfs`, `resize2fs` | for R1 and image inspection |

## Linux toolbox

`tools/docker/Dockerfile` → image `hibreak-tools` (Debian bookworm). Contains
`lpunpack`/`lpmake`/`lpdump`/`lpadd` built from AOSP sources, `e2fsprogs`,
`util-linux`, `simg2img`/`img2simg`, `squashfs-tools`, `binutils`, `python3`,
and a populated `locate` database — `unpack.sh` probes for lpunpack with
`locate` and bails if it finds nothing.

Rebuild with `tools/docker/build.sh`.

**Loop-mounting an image that lives on the macOS bind mount was tested and
works** (Docker Desktop 28.5.2, virtiofs). So the super image can stay in
`./work` — no need to copy 12 GB into the VM. Verified with a 32 MB ext4 image
on both tmpfs and `/work`.

Phase 2 therefore looks like:

```bash
bin/hibreak-shell
# inside: /work is ./work, /scripts is the loopback7084 repo
cd /work/super && /scripts/unpack.sh super.bin
```

`unpack.sh` hard-checks the super size against `12884901888` (12 GiB) and exits
if it differs — expected for this device, but if our dump differs, that check is
telling us something, don't just edit it out.

## Pins

| Repo | Commit | Dated |
| --- | --- | --- |
| bkerler/mtkclient | `cd25cf9` | 2026-09-12 |
| loopback7084/hibreak_pro_color_scripts | `f49f973` | 2025-10-20 |
| LonelyFool/lpunpack_and_lpmake (`tools/otatools-src`) | `7ec860c` | 2021-11-15 |

mtkclient reports itself as *MTK Flash/Exploit Client V2.1.4*.

Everything under `tools/` except the scripts (`bootstrap.sh`, `env.sh`,
`docker/`, `otatools/build-macos.sh`) is gitignored — those are upstream
checkouts and build output, reconstituted by `bootstrap.sh`.

## Build notes (so nobody re-derives these)

`tools/otatools/build-macos.sh` clones, patches and builds the native binaries.
Upstream `make.sh` does not build on a modern macOS toolchain; three fixes were
needed, none of which apply on Linux (the container builds it unpatched):

1. `CFLAGS=-static` — `-static` doesn't link on macOS. Dropped.
2. `liblog/event_tag_map.cpp` uses `std::unary_function`, gone from modern
   libc++. Added `-D_LIBCPP_ENABLE_CXX17_REMOVED_UNARY_BINARY_FUNCTION`
   `-D_LIBCPP_ENABLE_CXX17_REMOVED_FEATURES`.
3. Bundled zlib's `zutil.h` sees `TARGET_OS_MAC` and `#define fdopen(fd,mode)
   NULL`, which then collides with the SDK's `stdio.h`. Worked around with
   `-Dfdopen=fdopen` on the zlib compile only.

Re-run with `tools/otatools/build-macos.sh`.

`mfusepy` is deliberately **not** installed in the venv. mtkclient imports it
for the `fs` command; without macFUSE it raises `OSError`, which mtkclient's
`except ImportError` doesn't catch, so *every* mtk.py invocation died. With the
package absent the import is a clean `ImportError` and mtkclient prints a
harmless `fuse library not installed` line. If `mtk fs` is ever wanted, that
means installing macFUSE (kext approval + reboot) — it is not needed for any
phase in CLAUDE.md.

The venv is python3.11, not the 3.14 that `python3` resolves to, purely for
wheel availability (`keystone-engine`, `unicorn`, `capstone`).

## Unverified

- **mtkclient BROM handshake on macOS has not been exercised** — no device has
  been connected yet. libusb loads and pyusb enumerates, but with nothing
  plugged in it sees 0 devices, which proves nothing. If BROM mode misbehaves on
  macOS, the fallback is running mtkclient from the Linux container with USB
  passthrough, which Docker Desktop does *not* support — that would mean a real
  Linux box or a UTM VM with USB passthrough. Find this out early, before
  relying on the macOS path for the 13–20 GB dump.
- `simg2img` is only in the container, not on the host. The mtkclient dumps
  should be raw, so it probably never comes up.
