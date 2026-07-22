#!/bin/sh
# Launch wrapper for a deployed Linux Lobbyd, execed by the systemd
# unit (sf4e-lobbyd.service). Reads its configuration from files beside
# it so updates can overwrite this script freely:
#
#   identity.txt      (required) the public address clients connect to,
#                     ex. sf4.zak123.com:23450
#   sidecar-hash.txt  pins the exact game build admitted to lobbies;
#                     absent = accept any build
#
# Crash-restart is systemd's job (Restart=always), so this execs
# instead of supervising.
cd "$(dirname "$0")" || exit 1

if [ ! -f identity.txt ]; then
	echo "identity.txt is missing- create it with this server's public address" >&2
	exit 1
fi
IDENTITY=$(head -n1 identity.txt | tr -d '[:space:]')

HASH=""
[ -f sidecar-hash.txt ] && HASH=$(head -n1 sidecar-hash.txt | tr -d '[:space:]')

# The GNS shared library and its deps ship beside the binary.
LD_LIBRARY_PATH="$(pwd)${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export LD_LIBRARY_PATH

if [ -n "$HASH" ]; then
	exec ./Lobbyd --port 23450 --no-default-lobby --identity "$IDENTITY" --sidecar-hash "$HASH"
else
	exec ./Lobbyd --port 23450 --no-default-lobby --identity "$IDENTITY"
fi
