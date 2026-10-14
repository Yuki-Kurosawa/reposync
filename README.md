# reposync

Cross-platform Debian repository mirror / sync tool, with a small SDL3 GUI.

`reposync` maintains a local mirror of Debian-style apt repositories. It
fetches `Release` + `Packages` indexes over libcurl (or plain HTTP), stores
the parsed data in a local SQLite database, and drives the repository
operations (`update`, `download`, `install`, `remove`, `upgrade`,
`autoremove`) through per-repository command sets configured with deb822
syntax. Everything below is project code — no apt/dpkg is invoked.

The GUI (SDL3 self-drawn, no widget toolkit) is a front-end for the CLI: it
runs the CLI locally on Linux, or over SSH (libssh2, no external `ssh`
client) from Windows. **The whole stack is free of GPL/LGPL** (SDL3 and
SDL3_ttf are zlib-licensed, libssh2 is BSD 3-clause, SQLite3 is public
domain).

## Features

- Index sync over HTTP/HTTPS: `Release`, `Packages` (and `Sources` when
  `Sync-Sources: yes`)
- deb822 configuration (`target.sources`, `sources.list.d/*.sources`,
  `reposync.conf`) — plain `sources.list` format is **not** accepted
- Suite mapping: packages follow their source suite, with a same-name /
  `<base>-*` variant rule (see below)
- Local SQLite queries: `list`, `sim install`, `db-import`, no daemon
- Per-repository operation command sets (switchable repository types via
  config)
- Windows support: CLI built with MSYS2, GUI built with MinGW-w64
- C99, no runtime dependency beyond the linked libraries

### Suite mapping rules

`reposync` keeps each package in the suite it came from, unless the target
has no such suite:

| Source suite → target suite | Allowed? |
|---|---|
| `abc/main` → `abc/main` | ✓ |
| `abc-updates/main` → `abc/main` | ✓ (only if the target has **no** `abc-updates`) |
| `abc/main` ↔ `abc/contrib` | ✗ (cross-component is banned in both directions) |
| `abc/main` → `abc-updates/main` | ✗ (never promoted to a variant suite) |

Packages that cannot be mapped are skipped with a warning on stderr.

## Build

The build is autotools (`configure` + automake). The top-level Makefile
dispatches into `src/` (CLI) and `gui/` (GUI, unless disabled).

### Linux / WSL

```sh
# CLI + GUI dependencies
#   Debian/Ubuntu: zlib1g-dev libzstd-dev libsqlite3-dev libcurl4-openssl-dev
#                  libsdl3-dev libsdl3-ttf-dev libssh2-1-dev
autoreconf -i
./configure        # --disable-gui for CLI only
make
```

Binaries land as `src/reposync`, `gui/reposync-gui`, `gui/selftest`.
`make distclean` removes everything including `.deps`.

### MSYS2 (Windows CLI)

```sh
pacman -S zlib zstd sqlite3 curl
./configure --disable-gui && make
# -> src/reposync.exe
```

### Windows GUI (MinGW-w64)

```sh
pacman -S mingw-w64-x86_64-sdl3 mingw-w64-x86_64-sdl3-ttf \
          mingw-w64-x86_64-libssh2 mingw-w64-x86_64-sqlite3
bash gui/win-build.sh          # make -f gui/Makefile.win + stage DLLs
```

The Windows GUI is always SSH mode (no local CLI on Windows): it connects to
a Linux host running `reposync` and executes operations there.

## Usage

1. Write `target.sources` (the repository being synced) and
   `sources.list.d/*.sources` (upstream sources); see `sources.example`.
2. Configure command sets and repo type in `reposync.conf`.
3. `reposync update` — fetch indexes, build the SQLite database.
4. `reposync list [pkg]`, `reposync install -y hello`, `reposync download`,
   `reposync remove`, `reposync upgrade`, `reposync sim install -y …`.

### Config paths

| Item | Linux | Windows |
|---|---|---|
| Global config | `/etc/reposync/reposync.conf` | `%EXEDIR%\reposync.conf` |
| Target repo file | `/etc/reposync/target.sources` | `%EXEDIR%\target.sources` |
| Upstream files | `/etc/reposync/sources.list.d/*.sources` | `%EXEDIR%\sources.list.d\*.sources` |
| SSH private key | `~/.ssh/id_*` | `%USERDIR%\.ssh\id_*` |

## Tests

Shell-based regression suites in the repository root (run from the project
directory; some need root/WSL):

- `cli-test.sh` — CLI happy path (update/download/install/remove/list/sim)
- `cli-http-test.sh` — sync + download over a local HTTP server
- `cli-suite-map-test.sh` — suite mapping rules (23 assertions)
- `cli-source-test.sh`, `mini-source-test.sh`, `quick-no-sources.sh` —
  `Sources` sync and edge cases
- `cli-chroot-test.sh` — chroot install path
- `gui-query-test.sh` — GUI headless selftest (needs the CLI built)

## Structure

```
reposync/
├── Makefile.am / configure.ac   # autotools, dispatches src/ + gui/
├── src/                         # CLI (C99): deb822, fetch, index, db, ops
├── gui/                         # SDL3 GUI + libssh2 backend, selftest
└── *.sh                          # build helpers (autogen/combo/cleanup) + tests
```

## License

[MIT](LICENSE) — Copyright (c) 2026 Yuki Kurosawa
