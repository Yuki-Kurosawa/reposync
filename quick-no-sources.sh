#!/bin/bash
# quick-no-sources.sh - regression: Sync-Sources: no must skip Sources and
# import only the plain binary closure (cl.changed)
set -u
cd /mnt/d/Repo/LinuxTestDrive/reposync || exit 1
[ "$(id -u)" = 0 ] || { echo "need root"; exit 1; }
R=/tmp/rt-nosrc
rm -rf $R
mkdir -p $R/conf/sources.list.d $R/lists $R/target/conf
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
Description: reposync no-sources test

EOF
cat > $R/conf/target.sources <<'EOF'
Types: deb
URIs: file:///tmp/rt-nosrc/target
Suites: trixie
Components: main
Architectures: amd64
Trusted: yes

EOF
cat > $R/conf/reposync.conf <<'EOF'
Active-Repo: reprepro
Sync-Sources: no

Repo-Type: reprepro
Path: /tmp/rt-nosrc/target
Import-Cmd: reprepro -b /tmp/rt-nosrc/target includedeb trixie
Source-Import-Cmd: reprepro -b /tmp/rt-nosrc/target includedsc trixie
Remove-Cmd: reprepro -b /tmp/rt-nosrc/target remove trixie
EOF
: > $R/status

export REPOSYNC_CONFIG_DIR=$R/conf/sources.list.d
export REPOSYNC_LIST_DIR=$R/lists
export REPOSYNC_TARGET=$R/conf/target.sources
export REPOSYNC_CONF=$R/conf/reposync.conf
export REPOSYNC_DB=$R/packages.db
export REPOSYNC_STATUS=$R/status
export REPOSYNC_MANUAL=$R/manual

./src/reposync update > $R/update.log 2>&1 || { echo "UPDATE FAILED"; tail -5 $R/update.log; exit 1; }
echo "cached indexes: $(ls $R/lists | sed 's/.*_//' | sort -u | tr '\n' ' ')"
ls $R/lists/*_Sources >/dev/null 2>&1 && { echo "FAIL: Sources fetched despite Sync-Sources: no"; exit 1; }
./src/reposync install -y app > $R/install.log 2>&1 || { echo "INSTALL FAILED"; tail -10 $R/install.log; exit 1; }
echo "NEW line: $(grep -E 'new, ' $R/install.log)"
grep -q "SOURCE packages" $R/install.log && { echo "FAIL: source closure ran despite Sync-Sources: no"; exit 1; }
B=$(zcat $R/target/dists/trixie/main/binary-amd64/Packages.gz 2>/dev/null | grep '^Package:' | awk '{print $2}' | sort)
echo "target binaries: $(echo "$B" | tr '\n' ' ')"
for p in app libx1 liby1; do
  echo "$B" | grep -qx "$p" || { echo "MISSING $p"; exit 1; }
done
echo "$B" | grep -qx "libx-dev" && { echo "FAIL: build-dep binary imported without sources"; exit 1; }
[ -z "$(find $R/target/pool -name '*.dsc' 2>/dev/null)" ] || { echo "FAIL: dsc imported without sources"; exit 1; }
echo "=== SYNC-SOURCES-NO REGRESSION PASS ==="
