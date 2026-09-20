#!/usr/bin/env bash
# 端到端测试：用 Debian trixie 官方源验证 update + download
set -uo pipefail
cd /mnt/d/Repo/LinuxTestDrive/reposync

T=$(mktemp -d /tmp/reposync-test.XXXXXX)
CFG="$T/sources.list.d"
LST="$T/lists"
DL="$T/dl"
mkdir -p "$CFG" "$LST" "$DL"

cat > "$CFG/debian.sources" <<'EOF'
Types: deb
URIs: http://deb.debian.org/debian
Suites: trixie
Components: main
Architectures: amd64
EOF

export REPOSYNC_CONFIG_DIR="$CFG"
export REPOSYNC_LIST_DIR="$LST"

echo "### UPDATE ###"
./reposync update
echo "RC=$?"
echo "### LISTS ###"
ls -la "$LST"

echo "### DOWNLOAD hello ###"
./reposync download hello -o "$DL"
echo "RC=$?"
echo "### DL DIR ###"
ls -la "$DL"

echo "### TEST DIR: $T ###"
