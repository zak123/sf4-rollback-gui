# Self-updater for a deployed Lobbyd. Run from a scheduled task every
# few minutes; see docs/vps-playtest.md.
#
# Checks the repo's latest GitHub release, and when it differs from the
# installed version, downloads the server bundle, stops the server,
# swaps the files, and starts it again. Configuration lives in files
# beside this script so updates can overwrite the script itself:
#
#   version.txt          (managed here) tag of the installed release
#   github-token.txt     (optional) a fine-grained PAT with read access
#                        to the repo's contents- required only while the
#                        repo is private; delete it once the repo is
#                        public
#   discord-webhook.txt  (optional) a Discord webhook URL to announce
#                        successful updates to

$repo = "zak123/sf4-rollback-gui"
$assetName = "sf4-rollback-gui-server.zip"
$taskName = "sf4e-lobbyd"

$ErrorActionPreference = "Stop"
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$dir = Split-Path -Parent $MyInvocation.MyCommand.Path

$headers = @{ "User-Agent" = "sf4e-lobbyd-updater" }
$tokenPath = Join-Path $dir "github-token.txt"
if (Test-Path $tokenPath) {
	$headers["Authorization"] = "Bearer $((Get-Content $tokenPath -Raw).Trim())"
}

# No releases yet, rate limited, or offline: quietly try again later.
try {
	$release = Invoke-RestMethod "https://api.github.com/repos/$repo/releases/latest" -Headers $headers
}
catch {
	exit 0
}

$tag = $release.tag_name
$installed = ""
$versionPath = Join-Path $dir "version.txt"
if (Test-Path $versionPath) {
	$installed = (Get-Content $versionPath -Raw).Trim()
}
if ($tag -eq $installed) {
	exit 0
}

$asset = $release.assets | Where-Object { $_.name -eq $assetName } | Select-Object -First 1
if (-not $asset) {
	exit 0
}

# Download and validate into temp before touching the live install.
$tmpZip = Join-Path $env:TEMP "sf4e-server-$tag.zip"
$tmpDir = Join-Path $env:TEMP "sf4e-server-$tag"
$dlHeaders = @{
	"User-Agent" = $headers["User-Agent"]
	"Accept" = "application/octet-stream"
}
if ($headers.ContainsKey("Authorization")) {
	$dlHeaders["Authorization"] = $headers["Authorization"]
}
Invoke-WebRequest $asset.url -Headers $dlHeaders -OutFile $tmpZip
Remove-Item $tmpDir -Recurse -Force -ErrorAction SilentlyContinue
Expand-Archive $tmpZip $tmpDir
if (-not (Test-Path (Join-Path $tmpDir "Lobbyd.exe"))) {
	Remove-Item $tmpZip, $tmpDir -Recurse -Force -ErrorAction SilentlyContinue
	exit 1
}

schtasks /end /tn $taskName 2>$null | Out-Null
Stop-Process -Name Lobbyd -Force -ErrorAction SilentlyContinue
Start-Sleep -Seconds 2
# A supervisor loop started outside the scheduled task (e.g. by hand)
# survives the /end above and respawns Lobbyd within seconds; catch a
# respawn right before the swap so the copy doesn't hit a locked exe.
Stop-Process -Name Lobbyd -Force -ErrorAction SilentlyContinue

try {
	Copy-Item (Join-Path $tmpDir "*") $dir -Force
	Set-Content $versionPath $tag -NoNewline -Encoding ascii
}
finally {
	# Always bring the server back, even if the swap threw- leaving it
	# stopped would turn a failed update into an outage.
	schtasks /run /tn $taskName | Out-Null
}

# Announce the update, if a webhook is configured. Announcement
# failures must never affect the update itself.
$webhookPath = Join-Path $dir "discord-webhook.txt"
if (Test-Path $webhookPath) {
	try {
		$webhook = (Get-Content $webhookPath -Raw).Trim()
		$msg = "Playtest server updated to $tag and restarted. " +
			"Everyone needs the matching client zip from this release: $($release.html_url)"
		$body = @{ content = $msg } | ConvertTo-Json
		Invoke-RestMethod -Method Post -Uri $webhook -ContentType "application/json" -Body $body | Out-Null
	}
	catch {}
}

Remove-Item $tmpZip, $tmpDir -Recurse -Force -ErrorAction SilentlyContinue
