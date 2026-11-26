#!/bin/bash
# One-time preparation from the Android APK (Spider-Man Total Mayhem Revived by RedMemories, v1.3).
# Nothing from the APK is part of this repository: it is unpacked next to it.
#   ./setup.sh /path/to/SpiderManTotalMayhem.apk
# Layout afterwards (next to this folder):
#   ../apk/SpiderManTotalMayhem.apk
#   ../x/lib/arm64-v8a/*.so           the three native libraries
#   ../data/SpiderManTotalMayhemRevived  the game data (gamedata.zip unpacked, ~370 MB)
#   ../java/sources/...               decompiled Java (only SoundDefines.java is used: the sound name table)
set -e
R=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$R")
APK=${1:-$ROOT/apk/SpiderManTotalMayhem.apk}
[ -f "$APK" ] || { echo "usage: $0 SpiderManTotalMayhem.apk"; exit 1; }
mkdir -p $ROOT/apk $ROOT/x $ROOT/data/SpiderManTotalMayhemRevived $ROOT/tools
[ "$APK" -ef $ROOT/apk/SpiderManTotalMayhem.apk ] || cp "$APK" $ROOT/apk/SpiderManTotalMayhem.apk
APK=$ROOT/apk/SpiderManTotalMayhem.apk

echo "== native libraries"
(cd $ROOT/x && unzip -oq $APK 'lib/*' res/drawable/icon.png)

echo "== game data"
TMP=$(mktemp -d)
unzip -oq $APK assets/gamedata.zip -d $TMP
(cd $ROOT/data/SpiderManTotalMayhemRevived && unzip -oq $TMP/assets/gamedata.zip)
rm -rf $TMP

echo "== sound name table (jadx)"
if [ ! -x $ROOT/tools/jadx/bin/jadx ]; then
	(cd $ROOT/tools && gh release download -R skylot/jadx -p "jadx-1*.zip" --clobber && unzip -oq jadx-*.zip -d jadx)
fi
JAVA_HOME=${JAVA_HOME:-$(/usr/libexec/java_home)} $ROOT/tools/jadx/bin/jadx -q -d $ROOT/java --no-res $APK || true
test -f $ROOT/java/sources/com/gameloft/android/GAND/GloftSMHP/ML/SoundDefines.java
echo "== done: ./build.sh sim  or  INSTALL=1 ./build.sh ios"
