#!/bin/bash
# cli-chroot-test.sh - "sync correctness" verification per 需求.md 验证方式:
#   往目标仓库里添加 bash -> 尝试展开系统 -> chroot 能看到 bash 提示符。
#
# The target repo is managed by ONE tool (reprepro): reposync's Import-Cmd
# IS reprepro's own import command (`reprepro includedeb trixie <deb>`), so
# pool placement AND index generation are both done by the same tool — no
# A-tool-import + B-tool-index mashup. reposync only syncs: update, resolve
# the dep closure, download, and hand each .deb to the configured manager.
#
# Run as root:  wsl -d Debian -u root -e bash cli-chroot-test.sh
set -u
cd /mnt/d/Repo/LinuxTestDrive/reposync || exit 1
[ "$(id -u)" = 0 ] || { echo "need root (chroot)"; exit 1; }
command -v reprepro >/dev/null || { echo "need reprepro"; exit 1; }
command -v dpkg-deb >/dev/null || { echo "need dpkg-deb"; exit 1; }

R=/tmp/rtc
rm -rf $R
mkdir -p $R/conf/sources.list.d $R/lists $R/dl $R/rootfs $R/target/conf

# ---- upstream: real Debian trixie over http ----
cat > $R/conf/sources.list.d/debian.sources <<'EOF'
Types: deb
URIs: http://deb.debian.org/debian
Suites: trixie
Components: main
Architectures: amd64
Trusted: yes

EOF

# ---- target repo managed by reprepro (same tool does import + index) ----
cat > $R/target/conf/distributions <<'EOF'
Origin: reposync-test
Label: reposync-test
Suite: trixie
Codename: trixie
Architectures: amd64
Components: main
Description: reposync sync-correctness test repo

EOF
cat > $R/conf/target.sources <<'EOF'
Types: deb
URIs: file:///tmp/rtc/target
Suites: trixie
Components: main
Architectures: amd64
Trusted: yes

EOF
cat > $R/conf/reposync.conf <<'EOF'
Active-Repo: reprepro

Repo-Type: reprepro
Path: /tmp/rtc/target
Import-Cmd: reprepro -b /tmp/rtc/target includedeb trixie
Remove-Cmd: reprepro -b /tmp/rtc/target remove trixie
EOF
: > $R/status

export REPOSYNC_CONFIG_DIR=$R/conf/sources.list.d
export REPOSYNC_LIST_DIR=$R/lists
export REPOSYNC_TARGET=$R/conf/target.sources
export REPOSYNC_CONF=$R/conf/reposync.conf
export REPOSYNC_DB=$R/packages.db
export REPOSYNC_STATUS=$R/status
export REPOSYNC_MANUAL=$R/manual

echo "=== 1 update from real Debian trixie (http) ==="
./reposync update || { echo "UPDATE FAILED"; exit 1; }

echo "=== 2 install bash -> reprepro imports + regenerates index ==="
./reposync install -y bash || { echo "INSTALL FAILED"; exit 1; }

echo "=== 3 target index written by reprepro ==="
ls -la $R/target/dists/trixie/main/binary-amd64/ $R/target/dists/trixie/Release
grep -E '^Package: (bash|libc6|libtinfo6)$' $R/target/dists/trixie/main/binary-amd64/Packages | sort
echo "pool files:"
find $R/target/pool -name '*.deb' | sort

echo "=== 4 target repo usable as a source (over http) ==="
mkdir -p $R/conf2/sources.list.d $R/lists2
cat > $R/conf2/sources.list.d/target.sources <<'EOF'
Types: deb
URIs: http://127.0.0.1:8891
Suites: trixie
Components: main
Architectures: amd64
Trusted: yes

EOF
python3 -m http.server 8891 --bind 127.0.0.1 --directory $R/target >/dev/null 2>&1 &
SRV=$!
trap "kill $SRV 2>/dev/null" EXIT
sleep 1
REPOSYNC_CONFIG_DIR=$R/conf2/sources.list.d REPOSYNC_LIST_DIR=$R/lists2 REPOSYNC_DB=$R/db2 \
  ./reposync update || { echo "TARGET-SOURCE UPDATE FAILED"; exit 1; }
REPOSYNC_DB=$R/db2 ./reposync list bash || { echo "TARGET-SOURCE LIST FAILED"; exit 1; }

echo "=== 5 expand a minimal system from the target repo ==="
awk '/^Package:/{print $2}' $R/target/dists/trixie/main/binary-amd64/Packages > $R/pkgs.txt
cat $R/pkgs.txt
N=0
while read -r p; do
  REPOSYNC_DB=$R/db2 REPOSYNC_LIST_DIR=$R/lists2 ./reposync download "$p" -o $R/dl >/dev/null || { echo "download $p failed"; exit 1; }
  N=$((N+1))
done < $R/pkgs.txt
echo "downloaded $N .deb(s), size:"
du -sh $R/dl
for f in $R/dl/*.deb; do dpkg-deb -x "$f" $R/rootfs || { echo "unpack $f failed"; exit 1; }; done
echo "rootfs /bin:"
ls $R/rootfs/bin/ 2>/dev/null | head -5

echo "=== 6 chroot must see a bash prompt ==="
mkdir -p $R/rootfs/dev
mknod -m 666 $R/rootfs/dev/null c 1 3 2>/dev/null || true
mknod -m 666 $R/rootfs/dev/zero c 1 5 2>/dev/null || true
PS1_OUT=$(chroot $R/rootfs /bin/bash -i -c 'printf "%s" "$PS1"' 2>/dev/null)
echo "chroot bash PS1=[$PS1_OUT]"
[ -n "$PS1_OUT" ] || { echo "FAIL: no bash prompt in chroot"; exit 1; }
chroot $R/rootfs /bin/bash -c 'echo "bash runs inside chroot"' || { echo "FAIL: bash cannot run"; exit 1; }

echo "=== CLI CHROOT TEST PASS (bash prompt: $PS1_OUT) ==="
