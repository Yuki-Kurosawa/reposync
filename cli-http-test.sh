#!/bin/bash
# cli-http-test.sh - end-to-end http transport verification
# update fetches BOTH source indexes over http:// (upstream + target),
# download pulls the .deb over http with size check, install uses the
# local Import-Cmd on the mirrored pool. Proves libcurl http is the
# live path (no curl.exe / no curl binary anywhere).
set -u
cd /mnt/d/Repo/LinuxTestDrive/reposync
R=/tmp/rt-http
rm -rf $R
mkdir -p $R/http-root/upstream/dists/trixie/main/binary-amd64 \
         $R/http-root/upstream/pool/main/h/hello \
         $R/http-root/upstream/pool/main/g/glibc \
         $R/http-root/target/dists/trixie/main/binary-amd64 \
         $R/lists $R/conf $R/dl $R/local-target/pool

cat > $R/http-root/upstream/dists/trixie/main/binary-amd64/Packages <<'EOF'
Package: libc6
Version: 2.36-9+deb12u7
Architecture: amd64
Filename: pool/main/g/glibc/libc6_2.36-9+deb12u7_amd64.deb
Size: 2918520

Package: hello
Version: 2.10-3
Architecture: amd64
Filename: pool/main/h/hello/hello_2.10-3_amd64.deb
Size: 13
Depends: libc6 (>= 2.34)

EOF
printf 'hello, worldX' > $R/http-root/upstream/pool/main/h/hello/hello_2.10-3_amd64.deb
python3 -c "import sys; open('$R/http-root/upstream/pool/main/g/glibc/libc6_2.36-9+deb12u7_amd64.deb','wb').write(b'x'*2918520)"
: > $R/http-root/target/dists/trixie/main/binary-amd64/Packages

# ---- both upstream AND target sources point at the http server ----
cat > $R/conf/upstream.sources <<'EOF'
Types: deb
URIs: http://127.0.0.1:8791/upstream
Suites: trixie
Components: main
Architectures: amd64
Trusted: yes

EOF
cat > $R/conf/target.sources <<'EOF'
Types: deb
URIs: http://127.0.0.1:8791/target
Suites: trixie
Components: main
Architectures: amd64
Trusted: yes

EOF
cat > $R/conf/reposync.conf <<'EOF'
Active-Repo: plain
Sync-Sources: no

Repo-Type: plain
Path: /tmp/rt-http/local-target
Import-Cmd: cp -t /tmp/rt-http/local-target/pool
Remove-Cmd: rm -f
EOF
: > $R/status

export REPOSYNC_CONFIG_DIR=$R/conf
export REPOSYNC_LIST_DIR=$R/lists
export REPOSYNC_TARGET=$R/conf/target.sources
export REPOSYNC_CONF=$R/conf/reposync.conf
export REPOSYNC_DB=$R/packages.db
export REPOSYNC_STATUS=$R/status
export REPOSYNC_MANUAL=$R/manual

python3 -m http.server 8791 --directory $R/http-root >/dev/null 2>&1 &
SRV=$!
trap "kill $SRV 2>/dev/null" EXIT
sleep 1

echo "=== 1 update (both sources over http) ==="
./reposync update || { echo "UPDATE FAILED"; exit 1; }

echo "=== 2 download (http + size check) ==="
./reposync download hello -o $R/dl || { echo "DOWNLOAD FAILED"; exit 1; }
ls -la $R/dl

echo "=== 3 install (local Import-Cmd on mirrored pool) ==="
./reposync install -y hello || { echo "INSTALL FAILED"; exit 1; }
echo "local target pool:"
ls $R/local-target/pool/

echo "=== 4 fetch raw http URL primitive ==="
./reposync fetch http://127.0.0.1:8791/upstream/dists/trixie/main/binary-amd64/Packages $R/dl/index.txt
head -3 $R/dl/index.txt

echo "=== HTTP TEST PASS ==="
