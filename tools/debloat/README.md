# Debloat scripts — our corrected fork

Derived from [loopback7084/hibreak_pro_color_scripts](https://github.com/loopback7084/hibreak_pro_color_scripts)
@ `f49f973`. Upstream is gitignored (`tools/hibreak_pro_color_scripts/`); **these
are the ones to run**. Full analysis of why in `work/research/debloat-review.md`.

Run them inside the Linux container — macOS cannot loop-mount ext4:

```bash
bin/hibreak-shell                 # ./work is /work
/work/../tools/debloat/...        # (see "Running" below)
```

## What we changed, and why

| # | Upstream | Ours | Why |
| --- | --- | --- | --- |
| 1 | `lpmake` without `--virtual-ab` | adds `--virtual-ab` | Our super's header declares `virtual_ab_device` and the device sets `ro.virtual_ab.enabled=true`. Repacking without it drops the flag. **Plausible brick.** |
| 2 | `--group main_b:0` | `--group main_b:12882804736` | Stock's group table gives `main_b` the *same* max size as `main_a`. `0` forbids any future `_b` allocation. |
| 3 | `rm *_b.img` after unpack | keeps `system_b.img` | `system_b` is **not** empty: it is a valid ext4 (volume "system") holding `system-other-odex-marker` — AOSP's `system_other` dexopt staging. 57 MB of real data. `product_b`/`system_ext_b`/`vendor_b` genuinely are empty and are still declared at size 0, matching stock. |
| 4 | `cp ./hosts.txt` with no such file | supplies `hosts.txt` | The file is absent from upstream. With no `set -e`, the `cp` failed and the script carried on **reporting success while doing nothing** — the telemetry half silently no-opped. |
| 5 | no `set -e` | `set -euo pipefail` everywhere | See above. A failing step must stop the run, not be papered over. |
| 6 | `locate lpunpack` | `command -v lpunpack` | `locate` needs a populated db and is a strange way to probe for a binary. |
| 7 | `sed -i` on signed APKs, always | opt-in via `PATCH_CAPTIVE_PORTAL=1`, default **off** | Rewriting bytes inside `*ResOverlay.apk` keeps the file size (both strings are 13 bytes) but **invalidates the APK signature**. If the platform then rejects the RRO the overlay is dropped. `hosts.txt` blocks the same domain without touching a signature. Opt in if you want upstream's behaviour. |

Not changed: **the removal list itself**. All 70 entries were resolved against
the 245 packages on this device and the e-ink stack survives intact — see
`debloat-review.md`. `xSettings` is not in it, which matters because it carries
`assets/apk/xMenu.apk`, the EInk Center installer.

## Running

From the host:

```bash
bin/hibreak-shell            # root shell, ./work at /work, this dir at /debloat
```

Then inside:

```bash
cd /work/super
/debloat/01-unpack.sh  /work/backup/out/super.bin   # or skip: already unpacked
/debloat/02-mount.sh
/debloat/03-debloat.sh
/debloat/04-unmount.sh
/debloat/05-repack.sh
```

Output: `/work/super/super.new.bin`. Flashing it is **RED** (§2.1):

```
bin/mtk w super work/super/super.new.bin --preloader work/backup/out/preloader_boot1.bin
```

### ⚠️ You MUST disable dm-verity, or it will not boot

`system`, `vendor`, `product` and `system_ext` are hashtree-protected. The
debloat changes their contents, so their verity root hashes no longer match and
the kernel refuses to mount them. Symptom: preloader starts, dies in ~4 s,
retries once, falls back to fastboot — with `slot-successful:a` still `yes` and
`slot-retry-count:a` still `7`, because LK never gets far enough to record a
failed slot.

Being unlocked does **not** save you. Unlocking relaxes AVB *signature*
checking; dm-verity is a separate kernel mechanism driven by the hashtree
descriptors and does not care about lock state. (Magisk's patched `boot` boots
fine without this, which is misleading — `boot` carries a *hash* descriptor, not
a hashtree.)

After flashing super, in fastboot — note the flags precede `flash`:

```
fastboot --disable-verity --disable-verification flash vbmeta_a        work/backup/out/vbmeta_a.bin
fastboot --disable-verity --disable-verification flash vbmeta_system_a work/backup/out/vbmeta_system_a.bin
fastboot --disable-verity --disable-verification flash vbmeta_vendor_a work/backup/out/vbmeta_vendor_a.bin
fastboot reboot
```

These write our own dumps back with two header flags flipped — content
identical to stock, only enforcement changes. Once set, the flags persist, so a
later GSI flash does not need to repeat this.

~17 minutes. Keep `work/backup/out/super.bin` — writing it back is the
documented recovery for a black screen (§9).

## Verified

`05-repack.sh` was round-tripped on the **unmodified** images from our own
`super.bin` and the result compared against stock with `lpdump`:

```
Metadata max size / slot count / Header flags   identical
partition table (8 entries, groups, attributes) identical
group table (default 0, main_a/main_b 12882804736 each)  identical
extent layout                                   identical
built size 12884901888                          == device size
```

So the corrected repack reproduces stock metadata exactly, including the
`virtual_ab_device` flag that upstream drops. Any difference after a real
debloat run will therefore be the *content* changes we intended, not geometry.

The 12 GB test artifact was deleted; re-run `05-repack.sh` to regenerate.

`lpmake` prints `Invalid sparse file format at header magic` once per image —
harmless, it probes for sparse format and falls back to raw.
