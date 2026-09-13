# Restoring the `vendor.xrz.*` property context on a GSI

**Status: verified working.** Fixes 51 SELinux denials. Does **not** fix the
tiled-display problem, which has a different cause.

## The defect

Bigme's HWC (`/vendor/lib64/hw/hwcomposer.mt6877.so`) keeps its runtime state in
`vendor.xrz.*` properties — `gray_256_dither`, `fps_limit`, `hwc_idle`. On a GSI
every write is refused:

```
avc: denied { set } for property=vendor.xrz.gray_256_dither
     scontext=u:r:hal_graphics_composer_default:s0
     tcontext=u:object_r:vendor_default_prop:s0
     tclass=property_service permissive=0
```

51 denials in a few minutes: 25 × `gray_256_dither`, 24 × `fps_limit`,
2 × `hwc_idle`.

**Cause:** Bigme declared the `vendor.xrz.*` property context in **system**
sepolicy. A GSI replaces system, so the mapping vanishes and the properties fall
through to `vendor_default_prop`, which the composer may not write. Vendor
properties belong in vendor policy — this is a vendor packaging bug, and it will
affect any Bigme e-ink device running any GSI.

## The trap

`vendor_sepolicy.cil` contains:

```
(allow hal_graphics_composer_default vendor_xrz_prop_31_0 (property_service (set)))
```

which looks like the answer. **It is not.** The `_31_0` suffix marks a *versioned
platform* type, present only in the compat `typeattributeset`. The base type is
not declared in vendor policy:

```
vendor_xrz_prop                    declared=0   total_refs=0
vendor_mtk_graphics_hwc_pid_prop   declared=1   total_refs=9
```

Adding a `property_contexts` line referencing `vendor_xrz_prop` makes `init`
fail to parse it and **the device does not boot** — it drops to the recovery
error screen. Verified the hard way.

## The fix

Reuse a genuine vendor type the composer may already set *and* read:

```sh
# appended to /vendor/etc/selinux/vendor_property_contexts
vendor.xrz.    u:object_r:vendor_mtk_graphics_hwc_pid_prop:s0
```

Both permissions exist in vendor policy already:

```
(allow hal_graphics_composer_default vendor_mtk_graphics_hwc_pid_prop (property_service (set)))
(allow hal_graphics_composer_default vendor_mtk_graphics_hwc_pid_prop (file (read getattr map open)))
```

SurfaceFlinger can read it too. Semantically ugly — it borrows a type named for
the HWC's pid property — but functionally correct. A cleaner fix declares a
dedicated type, which needs policy recompilation.

## Applying it

`vendor` is a logical partition, so this is a ~670 MB fastboot write, not a
12 GB super write:

```sh
# build (macOS host + bin/hibreak-shell)
cp work/super/vendor_a.img work/super/vendor_a-xrzfix2.img
bin/hibreak-shell bash -c '
  cd /work/super
  dd if=/dev/zero bs=1M count=16 status=none >> vendor_a-xrzfix2.img
  e2fsck -fy vendor_a-xrzfix2.img; resize2fs vendor_a-xrzfix2.img
  mkdir -p /mnt/v && mount -o loop,rw vendor_a-xrzfix2.img /mnt/v
  echo "vendor.xrz.    u:object_r:vendor_mtk_graphics_hwc_pid_prop:s0" \
    >> /mnt/v/etc/selinux/vendor_property_contexts
  umount /mnt/v
  e2fsck -fy vendor_a-xrzfix2.img; resize2fs -M vendor_a-xrzfix2.img
  e2fsck -fy vendor_a-xrzfix2.img'

# flash (RED)
fastboot reboot fastboot
fastboot flash vendor work/super/vendor_a-xrzfix2.img
```

Requires dm-verity already disabled (see CLAUDE.md §6.2). Revert with
`fastboot flash vendor work/super/vendor_a.img`.

**Always verify the type is declared before flashing:**

```sh
grep -E "^\(type <TYPE>\)" vendor_sepolicy.cil
```

## Result

| | before | after |
| --- | --- | --- |
| `avc denied … xrz` | 51 | **0** |
| `vendor.xrz.gray_256_dither` | unset | `0` (composer set it; matches stock) |
| `vendor.xrz.fps_limit` | unset | `0` (matches stock) |
| `vendor.xrz.hwc_idle` | unset | `1` (matches stock) |
| layer composition | 100% CLIENT | 100% CLIENT (**unchanged**) |
| tiled display | yes | **yes — not fixed by this** |
