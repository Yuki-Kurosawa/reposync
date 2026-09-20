#!/bin/bash
# http-test.sh - verify libcurl HTTP index fetch + HTTP-first URI preference
set -u
cd /mnt/d/Repo/LinuxTestDrive/reposync

echo "=== 0. rebuild test env ==="
bash cli-test.sh >/dev/null 2>&1

echo "=== 0. start local http server on 18080 ==="
cd /tmp/rt/upstream
python3 -m http.server 18080 >/dev/null 2>&1 &
SRVPID=$!
sleep 1

cd /mnt/d/Repo/LinuxTestDrive/reposync
mkdir -p /tmp/rt/conf-http
cat > /tmp/rt/conf-http/only.sources <<'EOF'
Types: deb
URIs: http://127.0.0.1:18080
Suites: trixie
Components: main
Architectures: amd64
Trusted: yes
EOF

cat > /tmp/rt/conf-http/mixed.sources <<'EOF'
Types: deb
URIs: file:///tmp/rt/upstream http://127.0.0.1:18080
Suites: trixie
Components: main
Architectures: amd64
Trusted: yes
EOF

export REPOSYNC_LIST_DIR=/tmp/rt/lists
export REPOSYNC_TARGET=/tmp/rt/conf/target.sources
export REPOSYNC_CONF=/tmp/rt/conf/reposync.conf
export REPOSYNC_STATUS=/tmp/rt/status

echo "=== 1. http-only source: libcurl fetches index over HTTP ==="
REPOSYNC_CONFIG_DIR=/tmp/rt/conf-http REPOSYNC_DB=/tmp/rt/http-only.db \
    ./reposync update 2>&1 | grep -E "got http|indexed"

echo "=== 2. HTTP-first ordering (mixed URIs: file listed first) ==="
rm -f /tmp/rt/conf-http/only.sources
REPOSYNC_CONFIG_DIR=/tmp/rt/conf-http REPOSYNC_DB=/tmp/rt/http-mixed.db \
    ./reposync update 2>&1 | grep "^Source:" | head -4

echo "=== 3. linked crypto deps (libcurl pulls OpenSSL) ==="
ldd ./reposync | grep -iE "libssl|libcrypto" || echo "(no ssl/crypto)"

kill $SRVPID 2>/dev/null
echo "=== HTTP TEST DONE ==="
