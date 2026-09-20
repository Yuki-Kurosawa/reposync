#!/usr/bin/env bash
# Which DEB822 Options syntax does apt 2.9.x accept for an unsigned repo?
set -u
cd "$(dirname "$0")"
T=$(mktemp -d /tmp/aptprobe.XXXXXX)
ROOT=$T/www; mkdir -p "$ROOT"
SRC=$(ls -dt /tmp/reposync-target.*/www/target 2>/dev/null | head -1)
[ -n "$SRC" ] || { echo "no kept test repo"; exit 1; }
cp -r "$SRC" "$ROOT/" || exit 1
python3 -m http.server 8937 --bind 127.0.0.1 --directory "$ROOT" >/dev/null 2>&1 &
SRV=$!
sleep 1

try() { # try <label> <line> <mode: options|top>
    local label=$1 line=$2 mode=$3
    local d=$T/$label
    mkdir -p "$d"
    if [ "$mode" = top ]; then
        cat > "$d/t.sources" <<EOF
Types: deb
URIs: http://127.0.0.1:8937/target
Suites: trixie
Components: main
Architectures: amd64
$line
EOF
    else
        cat > "$d/t.sources" <<EOF
Types: deb
URIs: http://127.0.0.1:8937/target
Suites: trixie
Components: main
Architectures: amd64
Options: $line
EOF
    fi
    local out
    out=$(sudo apt-get -o Dir::Etc::sourcelist=/dev/null -o Dir::Etc::sourceparts="$d" update 2>&1)
    if echo "$out" | grep -q 'not signed'; then
        echo "$label: FAIL ($line)"
    else
        echo "$label: OK   ($line)"
    fi
}

try a "trusted=yes" options
try b "Trusted=yes" options
try c "allow-insecure=yes" options
try e "Trusted: yes" top
try f "trusted: yes" top

kill "$SRV" 2>/dev/null
rm -rf "$T"
