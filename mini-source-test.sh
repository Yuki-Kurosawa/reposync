#!/bin/bash
# mini-source-test.sh - Sources sync verification on a MINI upstream repo
# (reprepro-managed, Architectures: amd64 source). 构造源码链验证双向闭包：
#   app  (Binary: app,       Build-Depends: libx-dev)
#   libx (Binary: libx1 libx-dev, Build-Depends: liby-dev)
#   liby (Binary: liby1 liby-dev, Build-Depends: (none))
# 预期闭包: 二进制 {app,libx1,libx-dev,liby1,liby-dev} 源码 {app,libx,liby}
set -u
cd /mnt/d/Repo/LinuxTestDrive/reposync || exit 1
[ "$(id -u)" = 0 ] || { echo "need root"; exit 1; }
command -v reprepro >/dev/null || { echo "need reprepro"; exit 1; }

R=/tmp/rt-mini
rm -rf $R
mkdir -p $R/up/conf $R/conf/sources.list.d $R/lists $R/dl $R/target/conf $R/build

# ---------- build mini upstream repo (reprepro) ----------
UP=$R/up
cat > $UP/conf/distributions <<'EOF'
Origin: mini
Label: mini
Suite: trixie
Codename: trixie
Architectures: amd64 source
Components: main
Description: mini upstream

EOF

# helper: make a binary .deb
mkdeb() { # name ver deps
  local d=$R/build/deb-$1
  rm -rf $d; mkdir -p $d/DEBIAN $d/usr/share/doc/$1
  {
    echo "Package: $1"; echo "Version: $2"; echo "Architecture: amd64"
    echo "Maintainer: Test <t@example.com>"
    echo "Section: main"; echo "Priority: optional"
    [ -n "${3:-}" ] && echo "Depends: $3"
    echo "Description: test binary $1"
  } > $d/DEBIAN/control
  echo "mini $1" > $d/usr/share/doc/$1/README
  dpkg-deb --build $d $R/build/$1_$2_amd64.deb >/dev/null
}

# helper: make a source tarball + .dsc
mksrc() { # name ver builddeps binarylist
  local d=$R/build/src-$1
  rm -rf $d; mkdir -p $d/$1-$2
  echo "mini source $1" > $d/$1-$2/README
  tar -C $d -czf $R/build/$1_$2.orig.tar.gz $1-$2
  local sz=$(stat -c%s $R/build/$1_$2.orig.tar.gz)
  local md5=$(md5sum $R/build/$1_$2.orig.tar.gz | cut -d' ' -f1)
  {
    echo "Format: 3.0 (quilt)"; echo "Source: $1"; echo "Binary: $4"
    echo "Architecture: any"; echo "Version: $2"
    echo "Maintainer: Test <t@example.com>"
    echo "Standards-Version: 4.6.0"
    echo "Section: main"; echo "Priority: optional"
    [ -n "${3:-}" ] && echo "Build-Depends: $3"
    echo "Files:"
    echo " $md5 $sz $1_$2.orig.tar.gz"
  } > $R/build/$1_$2.dsc
}

# liby: no build-deps, builds liby1 + liby-dev
mksrc liby 2.0 "" "liby1, liby-dev"
mkdeb liby1 2.0 ""
mkdeb liby-dev 2.0 "liby1"
# libx: build-depends on liby-dev, builds libx1 + libx-dev
mksrc libx 1.0 "liby-dev" "libx1, libx-dev"
mkdeb libx1 1.0 "liby1"
mkdeb libx-dev 1.0 "libx1"
# app: build-depends on libx-dev, builds app, runtime depends on libx1
mksrc app 1.0 "libx-dev" "app"
mkdeb app 1.0 "libx1"

for p in liby libx app; do
  reprepro -b $UP includedsc trixie $R/build/${p}_*.dsc >/dev/null || { echo "includedsc $p FAILED"; exit 1; }
  for f in $R/build/${p}*_amd64.deb; do
    [ -e "$f" ] && reprepro -b $UP includedeb trixie "$f" >/dev/null || { echo "includedeb $f FAILED"; exit 1; }
  done
done
echo "upstream: $(grep -c '^Package:' $UP/dists/trixie/main/binary-amd64/Packages 2>/dev/null) bins, $(grep -c '^Package:' $UP/dists/trixie/main/source/Sources 2>/dev/null) srcs"

# ---------- reposync target config ----------
cat > $R/conf/sources.list.d/mini.sources <<'EOF'
Types: deb
URIs: file:///tmp/rt-mini/up
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
Description: reposync mini test repo

EOF
cat > $R/conf/target.sources <<'EOF'
Types: deb
URIs: file:///tmp/rt-mini/target
Suites: trixie
Components: main
Architectures: amd64
Trusted: yes

EOF
cat > $R/conf/reposync.conf <<'EOF'
Active-Repo: reprepro
Sync-Sources: yes

Repo-Type: reprepro
Path: /tmp/rt-mini/target
Import-Cmd: reprepro -b /tmp/rt-mini/target includedeb trixie
Source-Import-Cmd: reprepro -b /tmp/rt-mini/target includedsc trixie
Remove-Cmd: reprepro -b /tmp/rt-mini/target remove trixie
EOF
: > $R/status

export REPOSYNC_CONFIG_DIR=$R/conf/sources.list.d
export REPOSYNC_LIST_DIR=$R/lists
export REPOSYNC_TARGET=$R/conf/target.sources
export REPOSYNC_CONF=$R/conf/reposync.conf
export REPOSYNC_DB=$R/packages.db
export REPOSYNC_STATUS=$R/status
export REPOSYNC_MANUAL=$R/manual

echo "=== 1 update (Sync-Sources: yes) ==="
./src/reposync update || { echo "UPDATE FAILED"; exit 1; }

echo "=== 2 install app (bidirectional closure) ==="
./src/reposync install -y app > $R/install.log 2>&1 || { echo "INSTALL FAILED"; tail -20 $R/install.log; exit 1; }
grep -E "NEW packages|SOURCE packages|new,|source" $R/install.log | head -6
echo "binary imported: $(grep -c '^  imported ' $R/install.log)"
echo "source imported: $(grep -c '^  imported source ' $R/install.log)"

echo "=== 3 assertions ==="
FAIL=0
T=$R/target
B=$(zcat $T/dists/trixie/main/binary-amd64/Packages.gz 2>/dev/null | grep '^Package:' | awk '{print $2}' | sort)
S=$(zcat $T/dists/trixie/main/source/Sources.gz 2>/dev/null | grep '^Package:' | awk '{print $2}' | sort)
echo "target binaries: $(echo "$B" | tr '\n' ' ')"
echo "target sources:  $(echo "$S" | tr '\n' ' ')"
for p in app libx1 libx-dev liby1 liby-dev; do
  echo "$B" | grep -qx "$p" || { echo "MISSING binary $p"; FAIL=1; }
done
for p in app libx liby; do
  echo "$S" | grep -qx "$p" || { echo "MISSING source $p"; FAIL=1; }
done
[ "$(find $T/pool -name '*.dsc' | wc -l)" -eq 3 ] || { echo "dsc count != 3"; FAIL=1; }
grep -A3 "SOURCE packages" $R/install.log | grep -q "liby" || { echo "closure missed liby"; FAIL=1; }

echo "=== 4 idempotent re-run ==="
OUT=$(./src/reposync install -y app 2>&1)
echo "$OUT" | tail -2
echo "$OUT" | grep -q "already in target repo" || { echo "NOT idempotent"; FAIL=1; }

[ $FAIL -eq 0 ] && echo "=== MINI SOURCES TEST PASS ===" || echo "=== MINI SOURCES TEST FAIL ==="
exit $FAIL
