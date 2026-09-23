#!/bin/bash
set -u
cd /mnt/d/Repo/LinuxTestDrive/reposync
R=/tmp/rt
rm -rf $R
mkdir -p $R/upstream/dists/trixie/main/binary-amd64 $R/upstream/pool/main/h/hello \
         $R/target/dists/trixie/main/binary-amd64 $R/target/pool \
         $R/lists $R/conf $R/dl

# ---- upstream Packages index (DEB822) ----
cat > $R/upstream/dists/trixie/main/binary-amd64/Packages <<'EOF'
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
printf 'hello, worldX' > $R/upstream/pool/main/h/hello/hello_2.10-3_amd64.deb
mkdir -p $R/upstream/pool/main/g/glibc
printf 'fake libc' > $R/upstream/pool/main/g/glibc/libc6_2.36-9+deb12u7_amd64.deb
# make the fake libc6 file exactly 2918520 bytes (Size check on download)
python3 -c "import sys; open('/tmp/rt/upstream/pool/main/g/glibc/libc6_2.36-9+deb12u7_amd64.deb','wb').write(b'x'*2918520)"

# ---- target repo: empty Packages index ----
: > $R/target/dists/trixie/main/binary-amd64/Packages

# ---- upstream source (DEB822 only) ----
cat > $R/conf/upstream.sources <<'EOF'
Types: deb
URIs: file:///tmp/rt/upstream
Suites: trixie
Components: main
Architectures: amd64
Trusted: yes

EOF

# ---- target.sources (apt view) ----
cat > $R/conf/target.sources <<'EOF'
Types: deb
URIs: file:///tmp/rt/target
Suites: trixie
Components: main
Architectures: amd64
Trusted: yes

EOF

# ---- reposync.conf: TWO repo types, switchable via Active-Repo ----
cat > /tmp/rt/rm.sh <<'EOF'
#!/bin/bash
# $1 = package name; drop its .deb from the target pool
find /tmp/rt/target/pool -name "$1_*.deb" -delete
EOF
chmod +x /tmp/rt/rm.sh

cat > $R/conf/reposync.conf <<'EOF'
Active-Repo: plain
Sync-Sources: no

Repo-Type: plain
Path: /tmp/rt/target
Import-Cmd: cp -t /tmp/rt/target/pool
Remove-Cmd: /tmp/rt/rm.sh

Repo-Type: plain-verbose
Path: /tmp/rt/target
Import-Cmd: cp -v -t /tmp/rt/target/pool
Remove-Cmd: /tmp/rt/rm.sh
EOF

: > $R/status

export REPOSYNC_CONFIG_DIR=$R/conf
export REPOSYNC_LIST_DIR=$R/lists
export REPOSYNC_TARGET=$R/conf/target.sources
export REPOSYNC_CONF=$R/conf/reposync.conf
export REPOSYNC_DB=$R/packages.db
export REPOSYNC_STATUS=$R/status
export REPOSYNC_MANUAL=$R/manual

echo "=== 0 help (paths) ==="
./src/reposync help | sed -n '1,20p'

echo "=== 1 update ==="
./src/reposync update || { echo "UPDATE FAILED"; exit 1; }

echo "=== 2 download ==="
./src/reposync download hello -o $R/dl || { echo "DOWNLOAD FAILED"; exit 1; }
ls -la $R/dl

echo "=== 3 install (Active=plain, exec: cp -t) ==="
./src/reposync install -y hello || { echo "INSTALL FAILED"; exit 1; }
echo "target pool:"
ls $R/target/pool/

echo "=== 4 remove (exec: rm.sh) ==="
./src/reposync remove -y hello || { echo "REMOVE FAILED"; exit 1; }
echo "target pool after remove:"
ls $R/target/pool/ 2>&1 || true

echo "=== 5 switch Active-Repo to plain-verbose (exec: cp -v -t) ==="
sed -i 's/Active-Repo: plain/Active-Repo: plain-verbose/' $R/conf/reposync.conf
./src/reposync install -y hello || { echo "INSTALL2 FAILED"; exit 1; }

echo "=== 6 sim install (db path) ==="
./src/reposync db-import
./src/reposync sim install -y hello || { echo "SIM FAILED"; exit 1; }

echo "=== 7 list (query) ==="
./src/reposync list || { echo "LIST FAILED"; exit 1; }
./src/reposync list hello | tail -2 || { echo "LIST HELLO FAILED"; exit 1; }

echo "=== ALL PASS ==="
