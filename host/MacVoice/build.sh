#!/bin/sh
set -eu
root="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
app="$root/build/MacVoice.app"
mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
cp "$root/Info.plist" "$app/Contents/Info.plist"
cp "$root/../../assets/fonts/Orbitron-Variable.ttf" "$app/Contents/Resources/Orbitron-Variable.ttf"
overlay="$(mktemp -d)"
trap 'rm -rf "$overlay"' EXIT
# Some Command Line Tools releases load two identical SwiftBridging module maps.
# Hide the duplicate so AppKit and Foundation can build.
if [ -f /Library/Developer/CommandLineTools/usr/include/swift/bridging.modulemap ]; then
    printf '// empty\n' > "$overlay/empty.modulemap"
    cat > "$overlay/overlay.yaml" << EOF
{
  "version": 0,
  "roots": [
    {
      "name": "/Library/Developer/CommandLineTools/usr/include/swift/bridging.modulemap",
      "type": "file",
      "external-contents": "$overlay/empty.modulemap"
    }
  ]
}
EOF
    set -- -vfsoverlay "$overlay/overlay.yaml"
else
    set --
fi
swiftc "$@" -parse-as-library -O -o "$app/Contents/MacOS/MacVoice" \
    "$root/Sources/MacVoiceApp.swift" \
    -framework SwiftUI -framework AppKit -framework CoreBluetooth \
    -framework Speech -framework AVFoundation -framework ApplicationServices \
    -framework CoreText
codesign --force --sign - "$app" >/dev/null
echo "built $app"
