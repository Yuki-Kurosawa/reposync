#!/usr/bin/env bash
# End-to-end with a real validating repo toolset (reprepro):
# - upstream A and target B are both reprepro-managed (normal toolset)
# - target.sources is a pure apt source; apt consumes B straight from it
# - reposync install/remove/upgrade/autoremove run through Import-Cmd/Remove-Cmd
#   (reprepro includedeb / remove), after computing the dependency-closure order.
set -u
cd "$(dirname "$0")"
SELF=$(pwd)
T=$(mktemp -d /tmp/reposync-target.XXXXXX)
CFG=$T/cfg; LST=$T/lists; ROOT=$T/www
UP=$ROOT/upstream; TG=$ROOT/target; PROVE=$ROOT/prove
mkdir -p "$CFG" "$LST" "$UP" "$TG"
PORT=8937
SRV=""

cleanup() {
    [ -n "$SRV" ] && kill "$SRV" 2>/dev/null
    [ -z "${KEEP:-}" ] && rm -rf "$T"
}
trap cleanup EXIT
[ -n "${KEEP:-}" ] && echo "KEEP=1: test dir $T"

mkdeb() { # mkdeb <dir> <name> <version> <arch> [deps...]  (PROV env sets Provides)
    local dir=$1 name=$2 ver=$3 arch=$4; shift 4
    local d=$dir/$name
    mkdir -p "$d/DEBIAN"
    {
        echo "Package: $name"
        echo "Version: $ver"
        echo "Architecture: $arch"
        echo "Maintainer: reposync <test@local>"
        echo "Section: utils"
        echo "Priority: optional"
        echo "Description: test package $name"
        [ -n "${PROV:-}" ] && { echo -n "Provides: "; echo "$PROV"; }
        [ $# -gt 0 ] && { echo -n "Depends: "; echo "$*"; }
    } > "$d/DEBIAN/control"
    dpkg-deb --build "$d" "$dir/${name}_${ver}_${arch}.deb" >/dev/null 2>&1
    rm -rf "$d"
}

mkdist() { # mkdist <repodir>
    mkdir -p "$1/conf"
    cat > "$1/conf/distributions" <<EOF
Origin: reposync
Label: reposync
Suite: trixie
Codename: trixie
Architectures: amd64
Components: main
EOF
}

echo "=== build upstream repo A with reprepro (normal toolset) ==="
mkdist "$UP"
D=$T/debs; mkdir -p "$D"
mkdeb "$D" libfoo     2.1-1 all
mkdeb "$D" libfoo-dev 2.1-1 all "libfoo (= 2.1-1)"
mkdeb "$D" libbar     1.4-1 all
mkdeb "$D" libbar-dev 1.4-1 all "libbar (= 1.4-1)"
mkdeb "$D" foobar     3.0-1 amd64 "libfoo (>= 2.0), libbar, libfoo-dev, libbar-dev"
PROV=qux-service mkdeb "$D" libqux 1.0-1 all
mkdeb "$D" app-qux    1.0-1 amd64 "qux-service"
mkdeb "$D" standalone 0.5-1 all
for f in "$D"/*.deb; do
    reprepro -b "$UP" includedeb trixie "$f" >/dev/null 2>&1 || { echo "reprepro rejected $f"; exit 1; }
done
reprepro -b "$UP" export trixie >/dev/null
echo "  upstream packages: $(reprepro -b "$UP" list trixie | wc -l)"

echo "=== init empty target repo B with reprepro ==="
mkdist "$TG"
reprepro -b "$TG" export trixie >/dev/null

echo "=== probe: reprepro with foobar alone (its own repo) ==="
mkdist "$PROVE"
if reprepro -b "$PROVE" includedeb trixie "$D/foobar_3.0-1_amd64.deb" >reprepro-reject.log 2>&1; then
    reprepro -b "$PROVE" export trixie >/dev/null
    echo "  (reprepro accepted foobar alone; dep check not on by default)"
else
    echo "  reprepro REFUSED foobar alone: $(grep -i 'cannot\|depend\|fail' reprepro-reject.log | head -1)"
fi

echo "=== serve repo root ==="
bash serve.sh "$ROOT" $PORT >/dev/null 2>&1 &
SRV=$!
sleep 1

echo "=== reposync config: upstream source + target (apt-clean) + target.conf ==="
cat > "$CFG/upstream.sources" <<EOF
Types: deb
URIs: http://127.0.0.1:$PORT/upstream
Suites: trixie
Components: main
Architectures: amd64
EOF
cat > "$T/target.sources" <<EOF
Types: deb
URIs: http://127.0.0.1:$PORT/target
Suites: trixie
Components: main
Architectures: amd64
Trusted: yes
EOF
cat > "$T/target.conf" <<EOF
Path: $TG
Import-Cmd: reprepro -b $TG includedeb trixie
Remove-Cmd: reprepro -b $TG remove trixie
EOF
cat > "$T/prove.sources" <<EOF
Types: deb
URIs: http://127.0.0.1:$PORT/prove
Suites: trixie
Components: main
Architectures: amd64
Trusted: yes
EOF
cat > "$T/prove.conf" <<EOF
Path: $PROVE
Import-Cmd: reprepro -b $PROVE includedeb trixie
Remove-Cmd: reprepro -b $PROVE remove trixie
EOF

export REPOSYNC_CONFIG_DIR=$CFG REPOSYNC_LIST_DIR=$LST \
       REPOSYNC_MANUAL=$T/manual
./reposync update || exit 1

echo
echo "=== reposync install foobar into empty B (closure, deps first) ==="
REPOSYNC_TARGET=$T/target.sources REPOSYNC_TARGET_CONF=$T/target.conf \
    ./reposync install -y foobar || exit 1
N=$(reprepro -b "$TG" list trixie | wc -l)
[ "$N" = "5" ] || { echo "expected 5 packages in target, got $N"; reprepro -b "$TG" list trixie; exit 1; }
for p in foobar libfoo libbar libfoo-dev libbar-dev; do
    reprepro -b "$TG" list trixie | grep -q "$p " || { echo "missing $p"; exit 1; }
done
echo "  closure ok: foobar libfoo libbar libfoo-dev libbar-dev"

echo
echo "=== repair: foobar alone in a repo, install completes it ==="
REPOSYNC_TARGET=$T/prove.sources REPOSYNC_TARGET_CONF=$T/prove.conf \
    ./reposync install -y foobar || exit 1
N=$(reprepro -b "$PROVE" list trixie | wc -l)
[ "$N" = "5" ] || { echo "expected 5 packages after repair, got $N"; reprepro -b "$PROVE" list trixie; exit 1; }
echo "  repo repaired to 5 packages"

echo
echo "=== apt consumes target.sources directly ==="
APT_CFG=$T/apt-sources
mkdir -p "$APT_CFG"
cp "$T/target.sources" "$APT_CFG/target.sources"
apto() { apt-get -o Dir::Etc::sourcelist=/dev/null -o Dir::Etc::sourceparts="$APT_CFG" "$@"; }
if ! sudo apt-get -o Dir::Etc::sourcelist=/dev/null -o Dir::Etc::sourceparts="$APT_CFG" update >apt-update.log 2>&1; then
    echo "apt update FAILED:"; tail -20 apt-update.log; exit 1
fi
echo "  apt-get update: OK (Release + Packages accepted)"
OUT=$(apto -s install foobar 2>/dev/null)
echo "$OUT" | grep -E '^Inst' | sed 's/^/  /'
N=$(echo "$OUT" | grep -c '^Inst')
[ "$N" = "5" ] || { echo "apt sees $N packages, expected 5"; exit 1; }
mkdir -p "$T/dl" && cd "$T/dl"
apto download foobar >/dev/null 2>&1 || { echo "apt download failed"; exit 1; }
echo "  apt download of foobar_3.0-1_amd64.deb: OK"
cd "$SELF"

echo
echo "=== reposync install app-qux -> virtual dep pulls provider libqux ==="
REPOSYNC_TARGET=$T/target.sources REPOSYNC_TARGET_CONF=$T/target.conf \
    ./reposync install -y app-qux || exit 1
reprepro -b "$TG" list trixie | grep -q libqux || { echo "provider libqux missing"; exit 1; }
echo "  virtual dep qux-service -> libqux imported"

echo
echo "=== reposync remove libbar refused (reverse dep) ==="
if REPOSYNC_TARGET=$T/target.sources REPOSYNC_TARGET_CONF=$T/target.conf \
        ./reposync remove -y libbar 2>remove-err.log; then
    echo "ERROR: remove libbar should have been refused"; cat remove-err.log; exit 1
fi
echo "  refused: $(cat remove-err.log)"

echo
echo "=== reposync remove foobar, then autoremove orphans ==="
REPOSYNC_TARGET=$T/target.sources REPOSYNC_TARGET_CONF=$T/target.conf \
    ./reposync remove -y foobar || exit 1
REPOSYNC_TARGET=$T/target.sources REPOSYNC_TARGET_CONF=$T/target.conf \
    ./reposync autoremove -y || exit 1
for p in libfoo libbar libfoo-dev libbar-dev; do
    reprepro -b "$TG" list trixie | grep -q "$p " && { echo "orphan $p still present"; exit 1; }
done
for p in app-qux libqux; do
    reprepro -b "$TG" list trixie | grep -q "$p " || { echo "manual/dependent $p wrongly removed"; exit 1; }
done
echo "  orphans removed: libfoo libbar libfoo-dev libbar-dev"
echo "  kept (explicit install + its provider): app-qux libqux"
reprepro -b "$TG" list trixie | sed 's/^/  /'

echo
echo "=== upgrade: re-install libfoo, bump upstream, reposync upgrade ==="
REPOSYNC_TARGET=$T/target.sources REPOSYNC_TARGET_CONF=$T/target.conf \
    ./reposync install -y libfoo || exit 1
mkdir -p "$T/debs2"
mkdeb "$T/debs2" libfoo 2.2-1 all
reprepro -b "$UP" includedeb trixie "$T/debs2/libfoo_2.2-1_all.deb" >/dev/null 2>&1 || exit 1
reprepro -b "$UP" export trixie >/dev/null
./reposync update >/dev/null || exit 1
REPOSYNC_TARGET=$T/target.sources REPOSYNC_TARGET_CONF=$T/target.conf \
    ./reposync upgrade -y || exit 1
reprepro -b "$TG" list trixie | grep -q "libfoo 2.2-1" || { echo "libfoo not upgraded to 2.2-1"; reprepro -b "$TG" list trixie; exit 1; }
echo "  libfoo upgraded to 2.2-1"

echo
echo "=== final apt re-check on upgraded repo ==="
sudo apt-get -o Dir::Etc::sourcelist=/dev/null -o Dir::Etc::sourceparts="$APT_CFG" update >/dev/null 2>&1 || { echo "apt re-update failed"; exit 1; }
apt-cache -o Dir::Etc::sourcelist=/dev/null -o Dir::Etc::sourceparts="$APT_CFG" policy libfoo 2>/dev/null | grep -q "2.2-1" || { echo "apt candidate for libfoo is not 2.2-1"; exit 1; }
echo "  apt re-check: libfoo candidate is 2.2-1 (upgraded repo visible)"
echo
echo "ALL TESTS PASSED"
