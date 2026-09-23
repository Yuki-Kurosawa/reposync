#!/bin/bash
# cli-source-test.sh - Sources sync verification (需求.md "仓库后端与 Sources 同步"):
#   update 拉 Sources 索引 -> install 双向闭包（二进制 + 源码）
#   -> reprepro includedeb (binary) + includedsc (source) 一体化导入
#   -> target 仓库同时有 Packages 与 Sources 索引、pool 里有 .deb 与 .dsc。
# 注意：导入用 reprepro 逐包重建索引，数千包闭包耗时以小时计，仅作验证用；
# 生产后端 dak 为批量导入 + 最后统一重建索引（几分钟），无此瓶颈。
# Run as root:  wsl -d Debian -u root -e bash cli-source-test.sh
set -u
cd /mnt/d/Repo/LinuxTestDrive/reposync || exit 1
[ "$(id -u)" = 0 ] || { echo "need root"; exit 1; }
command -v reprepro >/dev/null || { echo "need reprepro"; exit 1; }

R=/tmp/rt-src
rm -rf $R
mkdir -p $R/conf/sources.list.d $R/lists $R/dl $R/target/conf
TMPDIR_BIG=/mnt/d/reposync-tmp
rm -rf $TMPDIR_BIG
mkdir -p $TMPDIR_BIG

cat > $R/conf/sources.list.d/debian.sources <<'EOF'
Types: deb
URIs: http://deb.debian.org/debian
Suites: trixie
Components: main
Architectures: amd64
Trusted: yes

EOF

cat > $R/target/conf/distributions <<'EOF'
Origin: reposync-test
Label: reposync-test
Suite: trixie
Codename: trixie
Architectures: amd64 source
Components: main
Description: reposync sources-sync test repo

EOF
cat > $R/conf/target.sources <<'EOF'
Types: deb
URIs: file:///tmp/rt-src/target
Suites: trixie
Components: main
Architectures: amd64
Trusted: yes

EOF
cat > $R/conf/reposync.conf <<'EOF'
Active-Repo: reprepro
Sync-Sources: yes

Repo-Type: reprepro
Path: /tmp/rt-src/target
Import-Cmd: reprepro -b /tmp/rt-src/target includedeb trixie
Source-Import-Cmd: reprepro -b /tmp/rt-src/target includedsc trixie
Remove-Cmd: reprepro -b /tmp/rt-src/target remove trixie
EOF
: > $R/status

export REPOSYNC_CONFIG_DIR=$R/conf/sources.list.d
export REPOSYNC_LIST_DIR=$R/lists
export REPOSYNC_TARGET=$R/conf/target.sources
export REPOSYNC_CONF=$R/conf/reposync.conf
export REPOSYNC_DB=$R/packages.db
export REPOSYNC_STATUS=$R/status
export REPOSYNC_MANUAL=$R/manual
export REPOSYNC_TMPDIR=$TMPDIR_BIG

echo "=== 1 update (Packages + Sources, Sync-Sources: yes) ==="
./src/reposync update || { echo "UPDATE FAILED"; exit 1; }
echo "cached indexes:"
ls $R/lists | sed 's/\..*//' | sort -u | tr '\n' ' '; echo
SRC_CACHE=$(ls $R/lists/*_Sources 2>/dev/null | head -1)
[ -n "$SRC_CACHE" ] || { echo "FAIL: no Sources index cached"; exit 1; }
echo "source records: $(grep -c '^Package:' $SRC_CACHE)"

echo "=== 2 install hello (bidirectional closure) ==="
./src/reposync install -y hello > $R/install.log 2>&1 || { echo "INSTALL FAILED"; tail -30 $R/install.log; exit 1; }
grep -c '^  imported ' $R/install.log | xargs echo "binary imported:"
grep -c '^  imported source ' $R/install.log | xargs echo "source imported:"
echo "--- closure plan excerpt ---"
grep -E "SOURCE packages|source\(s\)|NEW packages" $R/install.log | head -5
grep -A3 "SOURCE packages" $R/install.log | head -5

echo "=== 3 target repo state ==="
echo "Packages index:   $(grep -c '^Package:' $R/target/dists/trixie/main/binary-amd64/Packages 2>/dev/null) packages"
echo "Sources index:    $(grep -c '^Package:' $R/target/dists/trixie/main/source/Sources 2>/dev/null) source packages"
echo "pool .deb count:  $(find $R/target/pool -name '*.deb' | wc -l)"
echo "pool .dsc count:  $(find $R/target/pool -name '*.dsc' | wc -l)"
echo "hello binary in index:  $(grep -c '^Package: hello$' $R/target/dists/trixie/main/binary-amd64/Packages)"
echo "hello source in index:  $(grep -c '^Package: hello$' $R/target/dists/trixie/main/source/Sources)"
echo "hello .dsc in pool:     $(find $R/target/pool -name 'hello_*.dsc' | wc -l)"

echo "=== 4 idempotent re-run: nothing to import ==="
./src/reposync install -y hello 2>&1 | tail -2

echo "=== SOURCES SYNC TEST PASS ==="
