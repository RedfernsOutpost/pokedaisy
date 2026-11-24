#!/bin/zsh
# PokeDaisy.app's launcher: the desktop frontend with its data folder in ~/Library/Application Support/PokeDaisy
# (roms/, saves/, states/, host.ini). It finds the cores beside itself.
DIR="${0:A:h}"
DATA="$HOME/Library/Application Support/PokeDaisy"
mkdir -p "$DATA/roms"
exec "$DIR/pokedaisy-host" --data "$DATA" "$@"
