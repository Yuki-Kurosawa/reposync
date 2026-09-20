#!/bin/bash
# cli-chroot-test.sh - "sync correctness" verification per 需求.md 验证方式:
#   往目标仓库添加包 -> 用官方 debootstrap 展开系统 -> chroot 看到 bash 提示符。
#
# Target repo is managed by ONE tool (reprepro): reposync's Import-Cmd IS
# reprepro's own import command, so pool placement + index generation are
# both done by the same tool. reposync only syncs: update, resolve closure,
# download, hand each .deb to the configured manager.
#
# The "required package list" for the expansion comes from debootstrap's own
# definition: --variant=minbase installs exactly the packages with
# Priority: important (get_debs), so we extract that list from the upstream
# index and reposync-install it into the target. (How the list is maintained
# for the future live-ISO is the user's own follow-up.)
#
# Run as root:  wsl -d Debian -u root -e bash cli-chroot-test.sh
set -u
cd /mnt/d/Repo/LinuxTestDrive/reposync || exit 1
[ "$(id -u)" = 0 ] || { echo "need root (chroot)"; exit 1; }
command -v reprepro >/dev/null || { echo "need reprepro"; exit 1; }
command -v debootstrap >/dev/null || { echo "need debootstrap"; exit 1; }

R=/tmp/rtc
umount -R $R/rootfs/dev 2>/dev/null || true
rm -rf $R
mkdir -p $R/conf/sources.list.d $R/lists $R/dl $R/target/conf

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

echo "=== 2 required list = debootstrap's minbase definition (Priority: required + apt) ==="
: > $R/important.txt
for f in $R/lists/*_Packages; do
  awk '/^Package:/{pkg=$2} /^Priority: required/{print pkg}' "$f"
done | sort -u > $R/important.txt
echo "apt" >> $R/important.txt
sort -u $R/important.txt -o $R/important.txt
echo "required+apt packages: $(wc -l < $R/important.txt)"
grep -E '^(bash|dpkg|coreutils|libc6|apt|debconf|tar|gzip)$' $R/important.txt | tr '\n' ' '; echo
# debootstrap (debian-common): minbase base="apt", and for trixie the
# Priority: required set is ALWAYS included (any non-buildd variant).
# bash is required, so the target system gets it automatically.
echo "sync list size: $(wc -l < $R/important.txt)"

echo "=== 3 install the list -> reprepro imports + regenerates index ==="
./reposync install -y $(cat $R/important.txt) > $R/install.log 2>&1 \
  || { echo "INSTALL FAILED"; tail -20 $R/install.log; exit 1; }
grep -c '^  imported ' $R/install.log | xargs echo "imported:"
tail -3 $R/install.log

echo "=== 4 target index written by reprepro ==="
ls -la $R/target/dists/trixie/main/binary-amd64/Packages
echo "packages in target index: $(grep -c '^Package:' $R/target/dists/trixie/main/binary-amd64/Packages)"
grep -E '^Package: (bash|dpkg|coreutils|libc6|apt|debconf)$' $R/target/dists/trixie/main/binary-amd64/Packages | sort | tr '\n' ' '; echo

echo "=== 5 debootstrap (official) expands a system from the target repo ==="
python3 -m http.server 8891 --bind 127.0.0.1 --directory $R/target >/dev/null 2>&1 &
SRV=$!
trap "kill $SRV 2>/dev/null" EXIT
sleep 1
# --no-check-gpg: reprepro test repo is unsigned (signing is the follow-up
# for the future live-ISO, same as the package-list maintenance).
# minbase = apt + Priority: required (per debian-common), so bash is inside.
debootstrap --no-check-gpg --variant=minbase --arch=amd64 \
  trixie $R/rootfs http://127.0.0.1:8891 \
  > $R/debootstrap.log 2>&1 || { echo "DEBOOTSTRAP FAILED"; tail -25 $R/debootstrap.log; exit 1; }
echo "debootstrap ok. rootfs /bin:"
ls $R/rootfs/bin/ 2>/dev/null | tr '\n' ' '; echo
du -sh $R/rootfs

echo "=== 6 chroot must see a bash prompt ==="
PS1_OUT=$(chroot $R/rootfs /bin/bash -i -c 'printf "%s" "$PS1"' 2>/dev/null)
echo "chroot bash PS1=[$PS1_OUT]"
[ -n "$PS1_OUT" ] || { echo "FAIL: no bash prompt in chroot"; exit 1; }
chroot $R/rootfs /bin/bash -c 'echo "bash runs inside debootstrap-expanded chroot"' || { echo "FAIL: bash cannot run"; exit 1; }

echo "=== CLI CHROOT TEST PASS (bash prompt: $PS1_OUT) ==="
