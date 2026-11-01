#!/bin/zsh
# Starts the wrapper in its CrossOver bottle with D3DMetal (run install.sh first).
#   BOTTLE  CrossOver bottle name or full path       (default: azimuth)
#   AZDIR   wrapper folder inside the bottle drive_c (default: Azimuth)
# Extra environment is passed through, e.g.  MTL_HUD_ENABLED=1 scripts/run.sh
#
# The launcher app exits as soon as this script returns, and macOS tears its
# launchd job down with it: a wine started as the app's child is killed before
# the wrapper appears (empty wine.log, no session in azimuth.log). So the wine
# process is owned by a LaunchAgent in the GUI session instead: this script
# registers and kickstarts it, and `run.sh --agent` is what the agent runs.
set -u
AGENT_LABEL=io.github.fladnag86.fh2-port.run
AGENT_PLIST=$HOME/Library/LaunchAgents/$AGENT_LABEL.plist
SELF=${0:A}
BOTTLE=${BOTTLE:-azimuth}
AZDIR=${AZDIR:-Azimuth}
# This port needs CrossOver on Wine 11.17 with D3DMetal 4.0b3 (CrossOver Preview).
# CrossOver 26.3 ships Wine 11.0 + D3DMetal 3.0, where azimuth.exe exits without a
# log, so use Preview when it is installed, whatever the launcher passed in.
CROSSOVER=${CROSSOVER:-/Applications/CrossOver.app}
if [ -x "/Applications/CrossOver Preview.app/Contents/SharedSupport/CrossOver/bin/wine" ]; then
  CROSSOVER="/Applications/CrossOver Preview.app"
fi
CX="$CROSSOVER/Contents/SharedSupport/CrossOver"
# a bare name is looked up in the default bottle folder; CrossOver also takes a full path
[[ $BOTTLE == /* ]] || BOTTLE="$HOME/Library/Application Support/CrossOver/Bottles/$BOTTLE"
A="$BOTTLE/drive_c/$AZDIR"
export CX_BOTTLE=$BOTTLE CX_GRAPHICS_BACKEND=d3dmetal ROSETTA_ADVERTISE_AVX=1

# the LaunchAgent side: start wine here, where the app's teardown cannot reach it
if [ "${1:-}" = --agent ]; then
  cd "$A" || { echo "no wrapper folder at $A" >&2; exit 1; }
  mkdir -p data/logs
  [ -f data/logs/fh2port.log ] && mv data/logs/fh2port.log data/logs/fh2port.prev.log
  "$CX/bin/wine" "C:\\${AZDIR//\//\\}\\azimuth.exe" > data/logs/wine.log 2>&1 &
  wine=$!
  # wine hands the wrapper to wineloader and this pid can exit at once: wait for
  # the wrapper to show up, then stay alive (and keep the job) while it runs
  for i in {1..180}; do pgrep -f 'azimuth\.exe' >/dev/null && break; sleep 1; done
  while pgrep -f 'azimuth\.exe' >/dev/null; do sleep 5; done
  wait $wine 2>/dev/null
  exit 0
fi

[ -d "$A" ] || { echo "no wrapper folder at $A" >&2; exit 1; }
mkdir -p "$A/data/logs" "$HOME/Library/LaunchAgents"
cat > "$AGENT_PLIST" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
	<key>Label</key><string>$AGENT_LABEL</string>
	<key>ProgramArguments</key><array>
		<string>/bin/zsh</string>
		<string>$SELF</string>
		<string>--agent</string>
	</array>
	<key>EnvironmentVariables</key><dict>
		<key>AZDIR</key><string>$AZDIR</string>
		<key>BOTTLE</key><string>$BOTTLE</string>
		<key>CROSSOVER</key><string>$CROSSOVER</string>
	</dict>
	<key>RunAtLoad</key><false/>
	<key>StandardOutPath</key><string>$A/data/logs/run.log</string>
	<key>StandardErrorPath</key><string>$A/data/logs/run.log</string>
</dict></plist>
PLIST
launchctl bootout "gui/$(id -u)/$AGENT_LABEL" 2>/dev/null
launchctl bootstrap "gui/$(id -u)" "$AGENT_PLIST" || { echo "could not register $AGENT_LABEL" >&2; exit 1; }
launchctl kickstart -k "gui/$(id -u)/$AGENT_LABEL" || { echo "could not start $AGENT_LABEL" >&2; exit 1; }
echo "started $AGENT_LABEL (bottle $BOTTLE, CrossOver ${CROSSOVER:t:r})"
