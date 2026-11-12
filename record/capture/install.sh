#!/bin/sh
# Installs nr-record's capture DLL into a game that has OptiScaler (for example from the ANR FG full package):
#   sh install.sh "<folder of the game's .exe>"
# Copies nr-record into OptiScaler/custom, sets FGNvngxReplacement=nr-record in OptiScaler.ini (the original is kept in
# OptiScaler/nr_record_backup), and forwards to ANR FG where it is installed, else to NVIDIA's DLSS-G.
set -e
game=${1:?usage: sh install.sh "<folder of the game's .exe>"}
here=$(cd "$(dirname "$0")" && pwd)
src="$here/capture/OptiScaler/custom/nr-record"
ini="$game/OptiScaler.ini"
if [ ! -d "$game/OptiScaler" ] || [ ! -f "$ini" ]; then
    echo "No OptiScaler in $game: install the ANR FG full package (the one with OptiScaler) there first. Nothing changed." >&2
    exit 1
fi
grep -q '^FGNvngxReplacement=' "$ini" || { echo "$ini has no FGNvngxReplacement line. Nothing changed." >&2; exit 1; }
dest="$game/OptiScaler/custom/nr-record"
backup="$game/OptiScaler/nr_record_backup"
mkdir -p "$backup"
[ -f "$backup/OptiScaler.ini" ] || cp -p "$ini" "$backup/OptiScaler.ini"   # the original, from before the first install
settings=""
[ -f "$dest/dlssg/nr_record.ini" ] && settings=$(mktemp) && cp "$dest/dlssg/nr_record.ini" "$settings"
rm -rf "$dest"
cp -R "$src" "$dest"
if [ -n "$settings" ]; then
    mv "$settings" "$dest/dlssg/nr_record.ini"
    forward="(kept your nr_record.ini)"
elif [ -f "$game/OptiScaler/custom/Apex Neural Rendering FG/dlssg/nvngx_dlssg.dll" ]; then
    sed -i 's|^forward =.*|forward = ..\\..\\Apex Neural Rendering FG\\dlssg\\nvngx_dlssg.dll|' "$dest/dlssg/nr_record.ini"
    forward="ANR FG"
else
    sed -i 's|^forward =.*|forward = nvidia|' "$dest/dlssg/nr_record.ini"
    forward="NVIDIA's DLSS-G"
fi
sed -i 's|^FGNvngxReplacement=[^\r]*|FGNvngxReplacement=nr-record|' "$ini"
echo "nr-record installed in $game:"
echo "  OptiScaler/custom/nr-record (the capture DLL), OptiScaler.ini: FGNvngxReplacement=nr-record"
echo "  when not recording, frames come from: $forward"
echo "  the original OptiScaler.ini: OptiScaler/nr_record_backup (sh uninstall.sh \"$game\" restores it)"
echo "Proton: the game's Steam launch options need: WINEDLLOVERRIDES=\"dxgi=n,b\" %command%"
