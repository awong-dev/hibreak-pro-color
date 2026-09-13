# Toolchain

Host: macOS (Apple Silicon, arm64). Set up 2026-09-12. See `CLAUDE.md` for the
mission and the GREEN/RED command rules — **this file does not relax any of
them**, and `STATE.md` for where the work actually is.

Status: everything here has been used in anger. The device has been dumped,
unlocked, and written to (§5.4). This is not a dry-run description.

## Fresh checkout

```bash
tools/bootstrap.sh       # idempotent; installs brew deps, clones, venv, image
source tools/env.sh      # bash or zsh — puts bin/, otatools, keg-only
                         # e2fsprogs and GNU coreutils on PATH
```

The upstream checkouts (`tools/mtkclient`, `tools/hibreak_pro_color_scripts`,
`tools/otatools-src`), the venv and the built binaries are all gitignored —
`bootstrap.sh` reconstitutes them. Only the scripts are tracked.

## Talking to the device — read this before running `bin/mtk`

This is the hardest-won knowledge in the project. Getting it wrong costs a
20-minute hang, not an error message.

**mtkclient hangs forever at "Uploading stage 2" in BROM mode on this device.**
Stage 2 runs from DRAM, which BROM has not initialised. mtkclient tries to
supply a DRAM config by matching the storage CID against its bundled preloaders
— but `xflash_lib.py`'s matching code is **eMMC-only**, and this is a **UFS**
device, so it matches dozens of wrong preloaders and every EMI send is rejected
(`DA exceed max num 0xc0070005`, on repeat). This will hit *every* UFS MediaTek
device. It is not macOS-specific.

Two ways round it, both proven here:

**1. Preloader mode (no extra files).** In preloader mode DRAM is already up, so
mtkclient skips the EMI step entirely (`elif connagent == b"preloader"` in
`xflash_lib.py`). Reaching it is the fiddly part:

- A **cold plug-in boots straight past** the preloader window into Android.
- What works is **warm**: start `bin/mtk <cmd>` so it is already polling, *then*
  `adb reboot`. mtkclient catches the preloader on the way up.
- Order matters. Poll first, reboot second.

**2. `--preloader` (preferred, now that we have it).** We dumped the preloader
from the UFS boot LUNs, and it carries an extractable **EMI v54** DRAM config —
exactly what mtkclient could not find on its own:

```bash
bin/mtk printgpt --preloader work/backup/out/preloader_boot1.bin
```

This makes **BROM mode work directly**, no reboot dance. It is also why that
file matters so much for recovery: without it, a corrupted preloader closes both
routes at once.

Other quirks worth knowing:

- After an operation mtkclient usually **leaves the device in DA mode**
  (`0x0E8D:0x2000`, "MT65xx Preloader"). Further `bin/mtk` commands often attach
  fine from there; if one refuses with *"Please disconnect, start mtkclient and
  reconnect"*, hold power 10–15 s, let it boot, and use the warm-reboot recipe.
- USB ids seen on this device: `0x0003` BROM · `0x2000` preloader/DA ·
  `0x2008` Android PTP-only · `0x201D` Android with adb · `0x201C` fastboot.
- Every `bin/mtk` run prints `fuse library not installed`. Harmless — see the
  `mfusepy` note below.

## Where things run

The original plan was "all image work in the Linux container". **In practice
almost none of it needed to be**, because `debugfs` reads ext4 read-only
*without mounting* and runs natively on macOS. The container is still the right
place for anything that genuinely mounts and writes.

| | Runs on | Why |
| --- | --- | --- |
| mtkclient, adb, fastboot, scrcpy | macOS host | needs USB |
| `lpunpack` / `lpdump` / `lpmake` | macOS host (native arm64) | also present in the container |
| **Reading inside ext4 images** (`debugfs -R "ls …"`, `dump`, `stat`) | **macOS host** | no mount, no root, no container — this is how the whole §6.2 review and the R2/R6 work was done |
| `mount -o loop` / `resize2fs` / `e2fsck` / the debloat proper | Linux container | macOS cannot loop-mount ext4 |
| `strings`, `file`, `binwalk`, `unsquashfs` | macOS host | analysis only |

So: reach for `debugfs` first. Only use `bin/hibreak-shell` when something has to
*modify* a filesystem image.

## Host commands

| Command | Notes |
| --- | --- |
| `bin/mtk <args>` | mtkclient wrapper — project venv (python3.11), keeps your cwd so relative output paths work. **`w`/`e`/`wl`/`wf`/`da` are RED.** See the section above before using it. |
| `bin/make-manifest.py` | regenerates `work/backup/MANIFEST.md` from `printgpt.txt` + `out/`. Reuses `SHA256SUMS` if present rather than rehashing 13 GB. Carries the standing analysis, so regenerating does not discard it. |
| `bin/hibreak-shell [cmd]` | root shell in the Linux toolbox, `./work` at `/work`, loopback7084 scripts read-only at `/scripts`. Builds the image on first use. **Not yet used for real work.** |
| `lpunpack`, `lpdump`, `lpmake`, `lpadd`, `lpflash` | native arm64, `tools/otatools/bin`. `lpunpack` does the whole 12 GiB super in ~10 s. |
| `debugfs` | keg-only, `/opt/homebrew/opt/e2fsprogs/sbin`. `debugfs -R "ls /system" img` / `-R "dump <path> <out>" img` / `-R "stat <path>" img`. A bogus path prints `File not found by ext2_lookup` — check for that, not for empty output. |
| `adb`, `fastboot` | platform-tools 37.0.1 |
| `scrcpy` | for §7.5 — start it *before* rebooting into a GSI. Not yet needed. |
| `binwalk` 3.1.0, `unsquashfs` | not actually needed so far; `strings` + `debugfs` covered R1/R2/R6 |

## Linux toolbox

`tools/docker/Dockerfile` → image `hibreak-tools` (Debian bookworm): `lpunpack`/
`lpmake`/`lpdump`/`lpadd` built from AOSP sources, `e2fsprogs`, `util-linux`,
`simg2img`/`img2simg`, `squashfs-tools`, `binutils`, `python3`, and a populated
`locate` database — `unpack.sh` probes for lpunpack with `locate` and bails if
it finds nothing. Rebuild with `tools/docker/build.sh`.

**Loop-mounting an image that lives on the macOS bind mount was tested and
works** (Docker Desktop 28.5.2, virtiofs), verified with a 32 MB ext4 image on
both tmpfs and `/work`. So the super images can stay in `./work` — no need to
copy 12 GB into the VM.

> ⚠️ **Do not run the loopback7084 scripts unmodified.** `repack.sh` omits
> `lpmake --virtual-ab`, which this device's super header requires, and silently
> drops `system_b`; `hosts.txt` is missing from the repo so half of
> `safedebloat.sh` no-ops without failing. Full analysis and the required fixes:
> `work/research/debloat-review.md`.

`unpack.sh` hard-checks the super size against `12884901888` (12 GiB).
**Verified: our dump is exactly that.** If a future dump differs, that check is
telling you something — don't edit it out.

## Pins

| Repo | Commit | Dated |
| --- | --- | --- |
| bkerler/mtkclient | `cd25cf9` | 2026-09-12 |
| loopback7084/hibreak_pro_color_scripts | `f49f973` | 2025-10-20 |
| LonelyFool/lpunpack_and_lpmake (`tools/otatools-src`) | `7ec860c` | 2021-11-15 |

mtkclient reports itself as *MTK Flash/Exploit Client V2.1.4*.

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

`mfusepy` is deliberately **not** installed in the venv. mtkclient imports it
for the `fs` command; without macFUSE it raises `OSError`, which mtkclient's
`except ImportError` doesn't catch, so *every* mtk.py invocation died. With the
package absent the import is a clean `ImportError` and mtkclient prints a
harmless `fuse library not installed` line. Installing macFUSE would mean kext
approval and a reboot, and no phase in CLAUDE.md needs `mtk fs`.

The venv is python3.11, not the 3.14 that `python3` resolves to, purely for
wheel availability (`keystone-engine`, `unicorn`, `capstone`).

## Verified on hardware

- **BROM/preloader over USB on macOS works.** BROM exploit succeeds and reports
  `SBC: False, SLA: False, DAA: False` — security is fully open on this unit.
  The Linux-box/UTM fallback once contemplated here is **not needed**.
- Full 13 GB dump, `mtk w` write-back, and fastboot all exercised. See `STATE.md`.

## Still unverified

- `simg2img` is only in the container, not on the host. The mtkclient dumps are
  raw, so it has not come up.
- `bin/hibreak-shell` has been smoke-tested (tools present, loop mount works) but
  **never used for actual image modification**. First real use will be the
  debloat, with the fixes above applied.
- `scrcpy` and `binwalk` are installed but unexercised.
