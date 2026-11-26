#!/bin/bash
# Builds Spider-Man: Total Mayhem for iOS from the RedMemories Android revival (APK in ../apk, extracted in ../x).
#   ./build.sh sim                 iOS Simulator build, installed into "iPhone 18 Pro" (data read from ../data)
#   ./build.sh ios                 device build -> ../dist/SpiderMan.ipa (game data bundled)
#   INSTALL=1 ./build.sh ios       also installs on the iPhone (never launches it)
#
# Your signing setup goes in local.env next to this script (not in git), for example:
#   SIGN_ID="Apple Development: Your Name (ABCDE12345)"
#   PROFILE="$HOME/Library/Developer/Xcode/UserData/Provisioning Profiles/<uuid>.mobileprovision"
#   TEAM_ID=ABCDE12345
#   BUNDLE_ID=com.yourname.smtm
#   DEVICE=<iPhone UDID for INSTALL=1>
#   DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer
set -e
R=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$R")
WHAT=${1:-sim}
BUNDLE_ID=com.example.smtm
SIM="iPhone 18 Pro"
[ -f $R/local.env ] && . $R/local.env
export DEVELOPER_DIR=${DEVELOPER_DIR:-$(xcode-select -p)}
if [ "$WHAT" = ios ] && { [ -z "$SIGN_ID" ] || [ -z "$PROFILE" ] || [ -z "$TEAM_ID" ]; }; then
	echo "device builds need SIGN_ID, PROFILE and TEAM_ID in $R/local.env (see the top of this script)"; exit 1
fi

if [ $WHAT = sim ]; then SDK=iphonesimulator; T=arm64-apple-ios17.0-simulator; else SDK=iphoneos; T=arm64-apple-ios17.0; fi
B=$ROOT/build/$WHAT
APP=$B/SpiderMan.app
mkdir -p $B/obj $ROOT/build/gen $ROOT/dist
rm -rf $APP; mkdir -p $APP/Frameworks

echo "== ELF images"
python3 $R/tools/elf2macho.py $ROOT/x/lib/arm64-v8a/libspiderman.so host $ROOT/build/gen \
	--export _ZN3mem6g_baseE,_ZNK8Dynarmic7Backend5Arm6411A32JitState4CpsrEv,_ZN8Dynarmic7Backend5Arm6411A32JitState7SetCpsrEj,_ZN3Cpu7currentEv,_ZN3Cpu7CallSVCEj,_ZN3Cpu15ExceptionRaisedEjN8Dynarmic3A329ExceptionE,_ZN3aot12direct_entryEj \
	--hook _ZN8Dynarmic3A323Jit3RunEv
python3 $R/tools/elf2macho.py $ROOT/x/lib/arm64-v8a/libspidermanaot.so aot $ROOT/build/gen
for n in host aot; do
	(cd $ROOT/build/gen && xcrun --sdk $SDK clang -target $T -dynamiclib $n.s -o $APP/Frameworks/libsmtm_$n.dylib \
		-install_name @rpath/libsmtm_$n.dylib -Wl,-segprot,__ELFX,rx,rx -Wl,-segprot,__ELFW,rw,rw)
done

echo "== runtime + app"
python3 - $ROOT/java/sources/com/gameloft/android/GAND/GloftSMHP/ML/SoundDefines.java $ROOT/build/gen/sound_names.h <<'PY'
import re, sys
src = open(sys.argv[1]).read()
arr = re.search(r'String\[\] a = \{(.*?)\};', src, re.S).group(1)
names = re.findall(r'"([^"]*)"', arr)
with open(sys.argv[2], 'w') as f:
    f.write('// generated from SoundDefines.a\n#define SOUND_COUNT %d\nstatic const char *g_soundNames[] = {\n' % len(names))
    f.write(''.join('    "%s",\n' % n for n in names) + '};\n')
PY
xcrun --sdk $SDK clang -target $T -isysroot $(xcrun --sdk $SDK --show-sdk-path) -O2 -w -c $R/third_party/stb_vorbis.c -o $B/obj/stb_vorbis.o &
CF="-target $T -isysroot $(xcrun --sdk $SDK --show-sdk-path) -O2 -g -Wall -Wno-unused-function -Wno-deprecated-declarations -I$R/runtime -I$ROOT/build/gen -I$R/third_party -DGLES_SILENCE_DEPRECATION"
for f in $R/runtime/*.c $R/runtime/*.m $R/runtime/*.S $R/app/*.m; do
	xcrun --sdk $SDK clang $CF -fobjc-arc -c $f -o $B/obj/$(basename $f).o &
done
wait
xcrun --sdk $SDK clang $CF $B/obj/*.o -o $APP/SpiderMan -L$APP/Frameworks -lsmtm_host -lsmtm_aot \
	-Wl,-rpath,@executable_path/Frameworks -fobjc-arc -fobjc-link-runtime -framework Foundation -framework UIKit -framework OpenGLES -framework QuartzCore -framework Metal -framework CoreVideo \
	-framework AVFoundation -framework GameController -framework CoreMotion
mkdir -p $APP/lib && cp $ROOT/x/lib/arm64-v8a/libspidermanarm32.so $APP/lib/   # the guest image (data for the runtime)
sed -e "s/__BUNDLE__/$BUNDLE_ID/; s/__BUILD__/$(date +%y%m%d%H%M)/" $R/res/Info.plist > $APP/Info.plist
# App icon: the APK's 120 px icon without its baked-in frame, made opaque (built from your APK, not stored here)
[ -f $ROOT/x/res/drawable/icon.png ] || (cd $ROOT/x && unzip -oq $ROOT/apk/SpiderManTotalMayhem.apk res/drawable/icon.png)
python3 -c "
from PIL import Image
i = Image.open('$ROOT/x/res/drawable/icon.png').convert('RGBA').crop((11, 11, 109, 109))
for size, name in ((180, '3x'), (120, '2x')):
    c = i.resize((size, size), Image.LANCZOS); bg = Image.new('RGB', c.size, (200, 30, 30)); bg.paste(c, (0, 0), c)
    bg.save('$APP/AppIcon60x60@' + name + '.png')"

if [ $WHAT = sim ]; then
	codesign -f -s - $APP/Frameworks/*.dylib $APP >/dev/null
	xcrun simctl boot "$SIM" 2>/dev/null || true
	xcrun simctl install "$SIM" $APP
	echo "== installed in simulator: SIMCTL_CHILD_SMTM_DATA=$ROOT/data/SpiderManTotalMayhemRevived xcrun simctl launch --console \"$SIM\" $BUNDLE_ID"
	exit 0
fi

echo "== data + sign"
cp -Rc $ROOT/data/SpiderManTotalMayhemRevived $APP/
cp "$PROFILE" $APP/embedded.mobileprovision
cat > $B/app.ent <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
	<key>application-identifier</key><string>$TEAM_ID.$BUNDLE_ID</string>
	<key>com.apple.developer.team-identifier</key><string>$TEAM_ID</string>
	<key>get-task-allow</key><true/>
</dict></plist>
EOF
codesign -f -s "$SIGN_ID" $APP/Frameworks/*.dylib
codesign -f -s "$SIGN_ID" --entitlements $B/app.ent $APP
rm -rf $B/ipa; mkdir -p $B/ipa/Payload; cp -Rc $APP $B/ipa/Payload/
(cd $B/ipa && rm -f $ROOT/dist/SpiderMan.ipa && zip -qry -0 $ROOT/dist/SpiderMan.ipa Payload)
echo "== $ROOT/dist/SpiderMan.ipa"
if [ "$INSTALL" = 1 ]; then xcrun devicectl device install app --device $DEVICE $APP | tail -2; fi
