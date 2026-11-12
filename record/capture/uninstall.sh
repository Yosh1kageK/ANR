#!/bin/sh
# Removes nr-record's capture DLL from a game: sh uninstall.sh "<folder of the game's .exe>"
# Restores the OptiScaler.ini install.sh backed up and deletes OptiScaler/custom/nr-record; OptiScaler stays.
set -e
game=${1:?usage: sh uninstall.sh "<folder of the game's .exe>"}
backup="$game/OptiScaler/nr_record_backup"
[ -d "$game/OptiScaler/custom/nr-record" ] || [ -d "$backup" ] || { echo "nr-record is not installed in $game." >&2; exit 1; }
if [ -f "$backup/OptiScaler.ini" ]; then
    cp -p "$backup/OptiScaler.ini" "$game/OptiScaler.ini"
    echo "restored OptiScaler.ini"
fi
rm -rf "$game/OptiScaler/custom/nr-record" "$backup"
echo "nr-record removed from $game (recordings in ~/.local/share/nr-record stay)"
