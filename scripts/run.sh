#!/bin/zsh
# Starts the wrapper in its CrossOver bottle with D3DMetal (run install.sh first).
#   BOTTLE  CrossOver bottle name or full path       (default: azimuth)
#   AZDIR   wrapper folder inside the bottle drive_c (default: Azimuth)
# Extra environment is passed through, e.g.  MTL_HUD_ENABLED=1 scripts/run.sh
set -u
BOTTLE=${BOTTLE:-azimuth}
AZDIR=${AZDIR:-Azimuth}
# CROSSOVER: the CrossOver app to use (the launcher also finds CrossOver Preview)
CROSSOVER=${CROSSOVER:-/Applications/CrossOver.app}
CX="$CROSSOVER/Contents/SharedSupport/CrossOver"
# a bare name is looked up in the default bottle folder; CrossOver also takes a full path
[[ $BOTTLE == /* ]] || BOTTLE="$HOME/Library/Application Support/CrossOver/Bottles/$BOTTLE"
export CX_BOTTLE=$BOTTLE CX_GRAPHICS_BACKEND=d3dmetal ROSETTA_ADVERTISE_AVX=1
A="$BOTTLE/drive_c/$AZDIR"
cd "$A" || { echo "no wrapper folder at $A" >&2; exit 1; }
mkdir -p data/logs
[ -f data/logs/fh2port.log ] && mv data/logs/fh2port.log data/logs/fh2port.prev.log
nohup "$CX/bin/wine" "C:\\${AZDIR//\//\\}\\azimuth.exe" > data/logs/wine.log 2>&1 &
echo "started pid $! (bottle $BOTTLE)"
