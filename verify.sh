#!/usr/bin/env bash
# 交叉验证：对比索引 SHA256 + 检查 .deb 内部结构
set -uo pipefail
cd /mnt/d/Repo/LinuxTestDrive/reposync

T=$(ls -dt /tmp/reposync-test.* 2>/dev/null | head -1)
if [ -z "$T" ]; then echo "no test dir found"; exit 1; fi
PKGS=$(ls "$T"/lists/*_Packages 2>/dev/null | head -1)
DEB=$(ls "$T"/dl/*.deb 2>/dev/null | head -1)
echo "test dir: $T"
echo "cache:    $PKGS"
echo "deb:      $DEB"

echo "=== SHA256 (index vs actual) ==="
IDX=$(awk '/^$/{f=0} /^Package: hello$/{f=1} f&&/^SHA256:/{print $2; exit}' "$PKGS")
ACT=$(sha256sum "$DEB" | awk '{print $1}')
echo "index:  $IDX"
echo "actual: $ACT"
[ "$IDX" = "$ACT" ] && echo "SHA256: MATCH" || echo "SHA256: MISMATCH"

echo "=== ar structure of .deb ==="
ar t "$DEB"
