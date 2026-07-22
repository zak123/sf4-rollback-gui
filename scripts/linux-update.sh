#!/bin/sh
# Self-updater for a deployed Linux Lobbyd. Run as root from the
# sf4e-update.timer systemd timer; mirrors vps-update.ps1: checks the
# repo's latest GitHub release, and when it differs from the installed
# version, downloads the Linux server tarball, validates it in temp,
# stops the service, swaps the files, and starts it again. Needs curl
# and jq. Files beside this script:
#
#   version.txt          (managed here) tag of the installed release
#   discord-webhook.txt  (optional) webhook URL to announce updates to
set -u

REPO="zak123/sf4-rollback-gui"
ASSET="sf4-rollback-gui-server-linux.tar.gz"
SERVICE="sf4e-lobbyd"

DIR=$(cd "$(dirname "$0")" && pwd)
cd "$DIR" || exit 0

# No releases yet, rate limited, or offline: quietly try again later.
RELEASE=$(curl -fsSL -H "User-Agent: sf4e-lobbyd-updater" \
	"https://api.github.com/repos/$REPO/releases/latest") || exit 0

TAG=$(printf '%s' "$RELEASE" | jq -r '.tag_name // empty')
[ -n "$TAG" ] || exit 0
INSTALLED=""
[ -f version.txt ] && INSTALLED=$(head -n1 version.txt | tr -d '[:space:]')
[ "$TAG" = "$INSTALLED" ] && exit 0

URL=$(printf '%s' "$RELEASE" | jq -r --arg n "$ASSET" \
	'.assets[] | select(.name == $n) | .browser_download_url' | head -n1)
[ -n "$URL" ] || exit 0

TMP=$(mktemp -d) || exit 0
trap 'rm -rf "$TMP"' EXIT

# Download and validate in temp before touching the live install: a
# bundle without the binary must never be deployed (this rule once
# saved the Windows box from a zip-of-zip release).
curl -fsSL -o "$TMP/$ASSET" "$URL" || exit 0
mkdir -p "$TMP/stage"
tar -C "$TMP/stage" -xzf "$TMP/$ASSET" || exit 0
[ -f "$TMP/stage/Lobbyd" ] || exit 0

# The pinned sidecar hash comes from the Windows build of the same
# release, published as its own asset. Absent = keep the current pin.
HASH_URL=$(printf '%s' "$RELEASE" | jq -r \
	'.assets[] | select(.name == "sidecar-hash.txt") | .browser_download_url' | head -n1)
if [ -n "$HASH_URL" ]; then
	curl -fsSL -o "$TMP/stage/sidecar-hash.txt" "$HASH_URL" || rm -f "$TMP/stage/sidecar-hash.txt"
fi

systemctl stop "$SERVICE"
cp -f "$TMP"/stage/* "$DIR"/
chmod +x "$DIR/Lobbyd" "$DIR"/*.sh 2>/dev/null
printf '%s\n' "$TAG" > "$DIR/version.txt"
systemctl start "$SERVICE"

if [ -f discord-webhook.txt ]; then
	WEBHOOK=$(head -n1 discord-webhook.txt | tr -d '[:space:]')
	[ -n "$WEBHOOK" ] && curl -fsS -X POST -H "Content-Type: application/json" \
		-d "{\"content\":\"sf4e Linux server updated to $TAG\"}" "$WEBHOOK" >/dev/null 2>&1
fi
exit 0
