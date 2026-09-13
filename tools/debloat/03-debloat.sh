#!/bin/bash
# Remove vendor/Google cruft from the mounted images and fix CN locale defaults.
#
# The removal list is VERBATIM from upstream f49f973. All 70 entries were
# resolved against the 245 packages on this device and the e-ink stack survives:
# com.xrz.sys.control, res.service, mutidisplay, standby, voice.text and
# ebook.launcher all stay, as do libgui.so, libXrzFramework_runtime.so,
# xrz_updater and /system/eink_key. See work/research/debloat-review.md.
#
# Note xSettings is deliberately NOT in the list: it carries assets/apk/xMenu.apk,
# which is what installs EInk Center on first boot.
set -euo pipefail
[ "$(id -u)" = "0" ] || { echo "needs root -- run inside bin/hibreak-shell"; exit 1; }
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

for d in system system_ext vendor product; do
    mountpoint -q "./$d" || { echo "./$d not mounted -- run 02-mount.sh first"; exit 1; }
done

list="system/system/app/OfflineTTs
system/system/app/XRZCalendar
system/system/app/ClockProd
system/system/app/CompanionDeviceManager
system/system/app/XRZ_OuLuDict
system/system/app/AppStroe
system/system/app/FileManager
system/system/app/XDict
system/system/app/XRZWebExport
system/system/app/XRZImageExplorer
system/system/app/BookSelf
system/system/app/ScanDOC
system/system/app/GooglePrintRecommendationService
system/system/app/PrintSpooler
system/system/app/BookMall
system/system/app/xReader
system/system/app/BasicDreams
system/system/app/xCloud
system/system/app/LOGCollect
system/system/app/xReaderPro
system/system/app/ColorDict
system/system/app/WifiTransferBook
system/system/app/CaptivePortalLoginGoogle
system/system/app/xMusicNew
system/system/app/Googletts
system/system/app/BK_Input
system/system/app/XRZSoundReocrd
system/system/app/TranSlator
system/system/app/ChatGPT
system/system/priv-app/MusicFX
system/system/priv-app/BuiltInPrintService
system/system/priv-app/Tag
system/system/priv-app/LiveWallpapersPicker
system/system/priv-app/CellBroadcastLegacyApp
system/system/preinstall
system_ext/app/Nfc_st
system_ext/priv-app/EmergencyInfoGms
system_ext/priv-app/VoiceCommand
system_ext/priv-app/GeofenceService
system_ext/priv-app/VoiceUnlock
product/app/Photos
product/app/YTMusic
product/app/YouTube
product/app/Keep
product/app/Gmail2
product/app/Videos
product/app/GoogleContacts
product/app/Drive
product/app/talkback
product/app/CalculatorGoogle
product/app/SpeechServicesByGoogle
product/app/SwitchAccess
product/app/CalendarGoogle
product/app/Chrome64
product/app/Meet
product/app/Maps
product/priv-app/SearchSelector
product/priv-app/AndroidSystemIntelligence
product/priv-app/Velvet
product/priv-app/PersonalSafety
product/priv-app/Messages
product/priv-app/PrivateComputeServices
product/priv-app/AssistantShell
product/priv-app/Wellbeing
product/priv-app/Turbo
system_ext/priv-app/MtkDialer
system_ext/priv-app/CallRecorderService
product/priv-app/GoogleDialer
system/system/priv-app/MtkMmsService
system_ext/app/AOVTestsApp"

ignore="
./system/system/app/XrzSettings
system/system/app/xLauncher3
system/system/app/Launcher
product/app/LatinImeGoogle

"

### Remove each entry. Missing paths are reported, not fatal: six of these do
### not exist on this firmware (LOGCollect, Tag, preinstall,
### SpeechServicesByGoogle, SearchSelector, MtkDialer).
removed=0; absent=0
for f in $list; do
    if [ -e "./$f" ]; then rm -rf "./$f"; removed=$((removed+1))
    else echo "  (absent, skipped) $f"; absent=$((absent+1)); fi
done
echo "removed $removed entries, $absent already absent"

### Telemetry. Upstream referenced a hosts.txt it did not ship.
install -m 0644 "$HERE/hosts.txt" ./system/system/etc/hosts
echo "installed hosts file"

### CN locale defaults -> en-GB.
BP=./system/system/build.prop
sed -i 's#ro.build.locale.area=http://ereader.xrztech.com:8090#ro.build.locale.area=http://localhost:8090#g' "$BP"
sed -i 's#ro.product.locale=zh-CN#ro.product.locale=en-GB#g'                                                "$BP"
sed -i 's#persist.sys.timezone=Asia/Shanghai#persist.sys.timezone=Europe/London#g'                          "$BP"
sed -i 's#persist.sys.country=CN#persist.sys.country=GB#g'                                                  "$BP"
sed -i 's#persist.sys.language=zh#persist.sys.language=gb#g'                                                "$BP"
echo "patched build.prop"

### Upstream also rewrote www.baidu.com inside three signed RRO APKs. Both
### strings are 13 bytes so the file size survives -- but the SIGNATURE does not,
### and a rejected overlay is silently dropped. hosts.txt covers the same ground
### without touching a signature, so this is opt-in.
if [ "${PATCH_CAPTIVE_PORTAL:-0}" = "1" ]; then
    for a in GoogleNetworkStackResOverlay NetworkStackInProcessResOverlay NetworkStackResOverlay; do
        t="./vendor/app/$a/$a.apk"
        [ -f "$t" ] && { sed -i 's#www.baidu.com#localhost:443#g' "$t"; echo "  patched $a (signature now invalid)"; }
    done
else
    echo "skipped captive-portal APK patching (set PATCH_CAPTIVE_PORTAL=1 to enable)"
fi
echo "done."
