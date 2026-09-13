# Building a LineageOS 23 GSI with phh/TrebleDroid patches, in a cloud VM

Target: an `arm64` LineageOS 23 (Android 16) GSI carrying the phh/TrebleDroid
Treble patches, flashable on the HiBreak Pro Color.

**This is a port, not a build.** Read §2 before committing money to a VM — the
work is rebasing a patch set onto a tree it was never written for, and the build
itself is the easy part.

---

## 1. Why this isn't turnkey

The maintained LineageOS-GSI-with-phh-patches project is
[AndyCGYan/lineage_build_unified](https://github.com/AndyCGYan/lineage_build_unified).
Verified 2026-09-13, it stops at LineageOS 22:

| repo | newest branch | we need |
| --- | --- | --- |
| `lineage_build_unified` | `lineage-22-light` | `lineage-23` |
| `lineage_patches_unified` | `lineage-22-light` | `lineage-23` |
| `AndyCGYan/android_device_lineage_gsi` | `lineage-22` | `lineage-23` |
| `TrebleDroid/vendor_hardware_overlay` | `pie` (version-agnostic) | — OK as-is |

LineageOS upstream has `lineage-23.0/23.1/23.2` and `lineage-24.0`. There is no
official LineageOS GSI device tree — the GSI target comes entirely from
AndyCGYan's `device/lineage/gsi`.

So the port is:

1. **`android_device_lineage_gsi`** `lineage-22` → `lineage-23`.
2. **75 patches**: 32 in `patches_platform`, 43 in `patches_treble`. Expect a
   meaningful fraction to fail on a one-release-newer tree.
3. **`repopick` IDs**. `build_unified.sh` cherry-picks LineageOS Gerrit changes
   `321337`, `321338`, `321339` (developer/USB notification tweaks). Those are
   LOS-22 change numbers and will not apply — find the equivalents or drop them.
4. Whatever Android 16 changed underneath: `gsi_arm64` target definitions,
   `aosp_target_release` handling, SELinux, `soong` config.

**Realistic estimate: a day or more of patch triage** by someone comfortable
reading AOSP build errors, before the first successful `make`.

---

## 2. Decide this first

Cheaper options that may already satisfy the goal:

- **`work/gsi/td16-system.img`** — TrebleDroid Android 16, already downloaded,
  phh patches and TrebleApp included. AOSP not LineageOS, but it is the same
  Treble patch lineage and it is *newer* than any buildable LOS+phh combination.
- **Build LineageOS 22 + phh instead** — `lineage-22-light` works today, no
  porting. Gets you Android 15.
- CLAUDE.md §8 records that **Lineage bases are harder for this panel** than
  AOSP: they need root + adb just to bring the display up on first boot, and
  setup wizard has been reported to hang. For objective 4 (panel driveable at
  all), AOSP is the lower-variable choice.

Build LOS 23 when you want a *daily driver* (objective 6), not while the panel
is still an open question.

---

## 3. Cloud VM

### Sizing

| | |
| --- | --- |
| vCPU | 16 minimum, 32 much better |
| RAM | 64 GB (AOSP wants ~4 GB/core; 32 GB works with `-j8` and swap) |
| Disk | **500 GB SSD.** Source ~150 GB, `out/` ~150–250 GB, ccache ~50 GB |
| OS | Ubuntu 24.04 LTS |

Disk is the hard requirement. Do not try this on 250 GB.

### GCP (you already have `gcloud` on PATH)

```bash
gcloud compute instances create los23-gsi \
  --zone=us-central1-a \
  --machine-type=n2-standard-32 \
  --boot-disk-size=500GB \
  --boot-disk-type=pd-balanced \
  --image-family=ubuntu-2404-lts-amd64 \
  --image-project=ubuntu-os-cloud
```

Rough cost, us-central1, on-demand: `n2-standard-32` ≈ **$1.55/hr**,
500 GB pd-balanced ≈ **$50/month** (≈ $0.07/hr). A sync + full build is
plausibly 6–10 h of VM time → **$10–20**, plus disk for as long as you keep it.

Use a **spot/preemptible** instance (`--provisioning-model=SPOT`) to cut compute
~60–70%, but checkpoint: AOSP builds resume fine, `repo sync` less so.

**Stop the instance between sessions** (`gcloud compute instances stop los23-gsi`)
— you keep paying for the disk but not the CPU.

AWS equivalent: `c7i.8xlarge` + 500 GB gp3. Azure: `Standard_D32ds_v5`.

### Connect

```bash
gcloud compute ssh los23-gsi --zone=us-central1-a
```

---

## 4. Environment

```bash
sudo apt update && sudo apt install -y \
  bc bison build-essential ccache curl flex g++-multilib gcc-multilib git \
  git-lfs gnupg gperf imagemagick lib32readline-dev lib32z1-dev libelf-dev \
  liblz4-tool libsdl1.2-dev libssl-dev libxml2 libxml2-utils lzop pngcrush \
  rsync schedtool squashfs-tools xsltproc zip zlib1g-dev python3 python-is-python3 \
  openjdk-21-jdk jq

mkdir -p ~/bin
curl https://storage.googleapis.com/git-repo-downloads/repo > ~/bin/repo
chmod a+x ~/bin/repo
export PATH=~/bin:$PATH

git config --global user.email "you@example.com"
git config --global user.name  "Your Name"
git lfs install

# ccache pays for itself on the second build
export USE_CCACHE=1 CCACHE_EXEC=/usr/bin/ccache
ccache -M 50G
```

---

## 5. Source

```bash
mkdir -p ~/los23-gsi && cd ~/los23-gsi
repo init -u https://github.com/LineageOS/android.git -b lineage-23.0 --git-lfs
```

Pick the branch deliberately: `lineage-23.0`, `23.1`, `23.2` track Android 16
QPR levels. Start at the one matching the QPR your vendor is happiest with —
**for this device, lowest is safest**, since the vendor is Android 12.

Clone the two helper repos at their newest branch; you will be editing both:

```bash
git clone https://github.com/AndyCGYan/lineage_build_unified   -b lineage-22-light
git clone https://github.com/AndyCGYan/lineage_patches_unified -b lineage-22-light
```

Local manifest (`.repo/local_manifests/manifest.xml`) — ported from
`lineage_build_unified/local_manifests_treble/manifest.xml`, with
`android_device_lineage_gsi` pointed at whatever branch you create in §6:

```xml
<?xml version="1.0" encoding="UTF-8"?>
<manifest>
  <project name="AndyCGYan/android_device_lineage_gsi" path="device/lineage/gsi"
           remote="github" revision="lineage-23" />
  <project name="AndyCGYan/android_packages_apps_QcRilAm" path="packages/apps/QcRilAm"
           remote="github" revision="master" />
  <project name="TrebleDroid/vendor_hardware_overlay" path="vendor/hardware_overlay"
           remote="github" revision="pie" />
  <remove-project name="LineageOS/android_packages_apps_Camera2" />
</manifest>
```

Drop the `MindTheGapps` entry unless you want a GApps build — we want vanilla
(`gsi_arm64_vN`), and fewer moving parts.

```bash
repo sync -c --force-sync --no-clone-bundle --no-tags -j$(nproc)
```

~150 GB, 1–3 h depending on network.

---

## 6. The port

### 6a. GSI device tree

```bash
git clone https://github.com/AndyCGYan/android_device_lineage_gsi -b lineage-22 gsi-tree
cd gsi-tree && git checkout -b lineage-23
```

Reconcile against Android 16's `gsi_arm64` target. Compare with AOSP's
`build/make/target/product/gsi_arm64.mk` and LineageOS's `vendor/lineage`
product definitions for 23. Push to your own fork and point the manifest at it.

### 6b. Patches

`apply_patches.sh` just walks a directory applying `.patch` files. Do it
manually the first time so you see each failure:

```bash
cd ~/los23-gsi
for p in lineage_patches_unified/patches_platform/*.patch; do
  echo "=== $p"; git apply --check "$p" 2>&1 | head -3
done
```

Triage each failure:
- **Trivial context drift** → `git apply -3` or `patch -p1 --fuzz=3`.
- **Upstreamed** → drop it. Check whether LOS 23 already has the change.
- **Genuinely rewritten area** → port by hand, or drop and note what you lost.

Keep a record of what you dropped. A patch you silently skipped is a feature
that silently doesn't work.

### 6c. repopick

```bash
source build/envsetup.sh
repopick 321337 -r -f   # will fail on LOS 23 — LOS 22 change numbers
```

Find the LOS 23 equivalents on [review.lineageos.org](https://review.lineageos.org)
or drop them; they are notification-behaviour niceties, not load-bearing.

---

## 7. Build

```bash
cd ~/los23-gsi
source build/envsetup.sh
source vendor/lineage/vars/aosp_target_release      # sets $aosp_target_release
lunch lineage_gsi_arm64_vN-${aosp_target_release}-userdebug
make installclean
WITH_ADB_INSECURE=true make -j$(nproc) systemimage
```

Output: `$OUT/system.img`.

`WITH_ADB_INSECURE=true` matters here — you will need adb to diagnose the panel,
and on a dark screen it is the only channel you have.

Targets, from `build_unified.sh`: `64VN` → `gsi_arm64_vN` (vanilla, no root),
`64GN` → `gsi_arm64_gN` (GApps). Build `vN`.

---

## 8. Retrieve

```bash
# on the VM
xz -T0 -9 $OUT/system.img          # ~2.2 GB -> ~700 MB
# from the Mac
gcloud compute scp los23-gsi:~/los23-gsi/out/target/product/*/system.img.xz \
  work/gsi/ --zone=us-central1-a
```

Then **stop or delete the instance.** A running n2-standard-32 left overnight
costs more than the whole build.

---

## 9. Flashing it here

Same as any GSI on this device — see `STATE.md` and CLAUDE.md §7.4. All RED:

```
adb reboot bootloader
fastboot reboot fastboot                    # fastbootd; system is logical
fastboot flash system work/gsi/system.img
fastboot -w
fastboot reboot
```

Revert:
`bin/mtk w super work/releases/super-debloat-v1.bin --preloader work/backup/out/preloader_boot1.bin`

Device facts the build must satisfy (all verified, see `device-identity.md`):

| | |
| --- | --- |
| arch | `arm64-v8a`, **`zygote64_32`** — the GSI must ship `/system/lib` as well as `lib64` |
| partitioning | virtual A/B, dynamic; `system` is logical → **fastbootd required** |
| vendor | Android **12**, `ro.vndk.version=31`, ships no VNDK of its own |
| `max-download-size` | 128 MiB — larger images sparse-split automatically |

⚠️ **VNDK**: no current GSI — TrebleDroid A15/A16 or Google's A17 — ships
`vndk-31`. They carry `vndk-28/29` or nothing. Whether the linker falls back
cleanly on this vendor is **untested**. If your LOS 23 build won't boot and the
TrebleDroid A16 image does, VNDK is the first place to look, and
`device/lineage/gsi` is where you would add a VNDK 31 snapshot.

---

## 10. What to expect on first boot

Per `work/research/eink-stack.md`, the panel driver, the CFA colour pipeline,
the waveform (loaded by **LK**, before Android) and six `drm_eink_*` ioctls all
live in the kernel and survive any GSI. What a GSI loses is Bigme's *policy*
layer — `libgui.so`'s four non-AOSP exports, `XrzEinkManager`, and
`xrz.framework.server.jar`.

So the expected first state is **a live device with a panel that never
refreshes**, not a dead one. Have `scrcpy` running *before* you reboot.

```bash
adb devices                                       # is it actually up?
adb shell service call SurfaceFlinger 1008 i32 1  # disable HW overlays
```

`drm_eink_update_ioctl` on `/dev/dri/card0` is the most promising route to
driving the panel from a GSI, and it is unexplored.
