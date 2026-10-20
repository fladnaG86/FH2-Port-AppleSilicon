#!/bin/zsh
# Installs the layer into a CrossOver bottle without modifying the wrapper.
#
#   scripts/install.sh [--settings] [--uninstall]
#   BOTTLE       CrossOver bottle name                    (default: azimuth)
#   AZDIR        wrapper folder inside the bottle drive_c (default: Azimuth)
#   BOTTLE_ROOT  folder holding the bottles   (default: CrossOver's standard one)
#   CX           CrossOver installation       (default: /Applications/CrossOver.app…)
#
# What it changes, all outside the wrapper's own files:
#   - copies build/d3d12.dll and build/fh2pad.dll next to azimuth.exe
#   - per-application DLL overrides for azimuth.exe (d3d12 native then builtin,
#     d3dcompiler_47 and the VC++ 2012 runtime native)
#   - registers fh2pad.dll as the provider of Windows.Gaming.Input.Gamepad
#   - WineBus DisableHidraw=1, so a DualSense is seen as a gamepad
#   - with --settings: antialiasing=off, resolution=1440 in data/azimuth.ini
#     (the user settings file, backed up first)
#   FH2_BUILD  folder with the two DLLs (default: build/ next to this script)
set -eu
BOTTLE=${BOTTLE:-azimuth}
AZDIR=${AZDIR:-Azimuth}
HERE=${0:A:h:h}
BUILD=${FH2_BUILD:-$HERE/build}
WINDIR=${AZDIR//\//\\}   # C:\ path of the wrapper folder, for nested folders too
BOTTLE_ROOT=${BOTTLE_ROOT:-$HOME/Library/Application Support/CrossOver/Bottles}
CX=${CX:-/Applications/CrossOver.app/Contents/SharedSupport/CrossOver}
export CX_BOTTLE_PATH="$BOTTLE_ROOT"
WINE="$CX/bin/wine"
DRIVE="$BOTTLE_ROOT/$BOTTLE/drive_c"
A="$DRIVE/$AZDIR"
APPKEY='HKCU\Software\Wine\AppDefaults\azimuth.exe\DllOverrides'
PADKEY='HKLM\Software\Microsoft\WindowsRuntime\ActivatableClassId\Windows.Gaming.Input.Gamepad'
BUSKEY='HKLM\System\CurrentControlSet\Services\WineBus'
# SHA-256 of the supported wrapper release (0.9.10), to warn about other versions
KNOWN_SHA=${KNOWN_SHA:-}

reg() { CX_BOTTLE=$BOTTLE "$WINE" reg "$@" >/dev/null 2>&1; }

[ -d "$A" ] && [ -f "$A/azimuth.exe" ] || { echo "no azimuth.exe in $A (set BOTTLE / AZDIR)" >&2; exit 1; }

if [ "${1:-}" = --uninstall ]; then
  reg delete "$APPKEY" /f || true
  reg add "$PADKEY" /v DllPath /t REG_SZ /d 'C:\windows\system32\windows.gaming.input.dll' /f
  rm -f "$A/d3d12.dll" "$A/fh2pad.dll"
  echo "removed the layer from $A"
  exit 0
fi

for f in d3d12.dll fh2pad.dll; do
  [ -f "$BUILD/$f" ] || { echo "$BUILD/$f missing: run make first" >&2; exit 1; }
done

sha=$(shasum -a 256 "$A/azimuth.exe" | cut -d' ' -f1)
if [ -n "$KNOWN_SHA" ] && [ "$sha" != "$KNOWN_SHA" ]; then
  echo "warning: azimuth.exe is not the tested release (sha256 $sha)" >&2
fi

cp "$BUILD/d3d12.dll" "$BUILD/fh2pad.dll" "$A/"
reg add "$APPKEY" /v d3d12 /t REG_SZ /d 'native,builtin' /f
reg add "$APPKEY" /v d3dcompiler_47 /t REG_SZ /d 'native,builtin' /f
reg add "$APPKEY" /v msvcr110 /t REG_SZ /d native /f
reg add "$APPKEY" /v msvcp110 /t REG_SZ /d native /f
reg add "$PADKEY" /v DllPath /t REG_SZ /d "C:\\$WINDIR\\fh2pad.dll" /f
reg add "$BUSKEY" /v DisableHidraw /t REG_DWORD /d 1 /f
echo "layer installed in $A"

[ -f "$A/d3dcompiler_47.dll" ] || echo "note: put Microsoft's d3dcompiler_47.dll in $A (see README)" >&2

if [ "${1:-}" = --settings ]; then
  ini="$A/data/azimuth.ini"
  [ -f "$ini" ] || { echo "no $ini yet: start the wrapper once, then run --settings" >&2; exit 1; }
  cp -p "$ini" "$ini.bak-fh2port"
  python3 - "$ini" <<'EOF'
import re, sys
p = sys.argv[1]
b = open(p, 'rb').read()
bom = b[:2] if b[:2] == b'\xff\xfe' else b''
t = b[len(bom):].decode('utf-16le')
for key, value in (('antialiasing', 'off'), ('resolution', '1440')):
    t, n = re.subn(r'(?m)^%s=.*$' % key, '%s=%s' % (key, value), t)
    if not n:
        t = t.replace('[launcher]', '[launcher]\r\n%s=%s' % (key, value), 1)
open(p, 'wb').write(bom + t.encode('utf-16le'))
EOF
  echo "settings: antialiasing=off, resolution=1440 (backup $ini.bak-fh2port)"
fi
