# reposync-gui

Graphical front-end for the `reposync` CLI (Debian repository mirror / sync
tool). It drives the CLI, so all operations are exactly the CLI's operations:
`update`, `download`, `install`, `remove`, `upgrade`, `autoremove` and the
`sim` family.

## UI & transports

The UI is **self-drawn on SDL3** (no widget toolkit): buttons, inputs, log
view, scrollback and a status bar are rendered directly with
`SDL_RenderGeometry`/`SDL_RenderLine`, with `SDL_ShowOpenFolderDialog` for the
directory picker. The SSH transport is **libssh2**, linked in — no external
`ssh` client is assumed anywhere (not even on Linux).

| Component | Library | License |
|---|---|---|
| UI / window / input / dialogs | SDL3 | zlib |
| Font rendering (CJK-capable) | SDL3_ttf | zlib (FreeType: FTL/BSD-like) |
| SSH transport | libssh2 | BSD 3-clause |
| Local index queries | SQLite3 | public domain |
| Everything else | C99, Win32/WinSock or POSIX | project code |

**The whole stack is free of GPL/LGPL.** GTK3, Qt and libssh were all
deliberately rejected because they are LGPL 2+; the GUI runs anywhere the CLI
is reachable — locally on Linux, or over SSH from Windows (where there is no
local CLI).

| Backend | Platform | What it does |
|---|---|---|
| `backend_local.c` | Linux | fork/exec `/bin/sh -c` and run `reposync` on this host |
| `backend_ssh2.c` (libssh2) | Linux + **Windows** | SSH to a Linux host and run `reposync` there |

On **Windows** there is no local CLI, so the GUI is always in SSH mode: it
connects to a Linux host that has `reposync` installed and executes the
operations remotely. libssh2 is therefore a hard dependency of the Windows
build; on Linux it is linked too (SSH mode is available from Linux as well),
so the same binary set covers both modes.

## Queries are local (no CLI process, no SSH)

`update` stores the fetched indexes into a local SQLite database — the same
design as apt's local lists. Queries therefore never spawn the CLI and never
touch the network, on Linux **and** Windows alike:

- **List** (`List` button, or type a package name first): reads the local
  database directly via SQLite. Linux: `/var/lib/reposync/packages.db`
  (or `$REPOSYNC_DB`); Windows: `%EXEDIR%\packages.db`.
- **分类查找 / Filter**: `Suite` + `Component` boxes query the local database
  with those filters (`WHERE suite=? AND component=?`), same code path.
- On Windows, after a remote **update** the GUI automatically pulls the remote
  `packages.db` back over SFTP (libssh2) to `%EXEDIR%\packages.db`, so all
  subsequent queries are offline-capable. The remote db path defaults to
  `/var/lib/reposync/packages.db` and is configurable via `remote_db_path`.

The local db schema is the CLI's `index_packages` table
(name/version/arch/suite/component/section/description), built by
`reposync update` (`db_candidates_rebuild`). Query rows are the same
tab-separated format as `reposync list`.

## Build

### Linux

```sh
sudo apt install build-essential pkg-config \
     libsdl3-dev libsdl3-ttf-dev libssh2-1-dev libsqlite3-dev   # Debian/Ubuntu
make -C gui            # builds reposync-gui and selftest
```

`make` uses `pkg-config --cflags/--libs sdl3 sdl3-ttf libssh2 sqlite3`.

### Windows (MSYS2)

```sh
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-sdl3 \
          mingw-w64-x86_64-sdl3-ttf mingw-w64-x86_64-libssh2 \
          mingw-w64-x86_64-sqlite3
bash -lc "export PATH=/mingw64/bin:\$PATH; \
          make -f Makefile.win LIBSSH2_ROOT=/mingw64 CC=gcc"
```

Note the package name is **lower-case `sdl3`**. `Makefile.win` builds with
`-mwindows`, keeps object files under a separate `.wino` suffix so the
Windows build never clobbers the Linux `.o` files, and strips the bogus
`.comment` section that some MinGW linkers emit (the Windows loader would
otherwise reject the exe).

**Runtime DLLs** — copy these next to `reposync-gui.exe` (from
`<msys2>/mingw64/bin`):

```
SDL3.dll            SDL3_ttf.dll          libssh2-1.dll
libfreetype-6.dll   libharfbuzz-0.dll     libgraphite2.dll
libglib-2.0-0.dll   libiconv-2.dll        libintl-8.dll
libbrotlidec.dll    libbrotlicommon.dll   libbz2-1.dll
libpng16-16.dll     libcrypto-3-x64.dll   zlib1.dll
libstdc++-6.dll     libwinpthread-1.dll   libgcc_s_seh-1.dll
libsqlite3-0.dll
```

(`objdump -p` on each DLL lists its imports; the list above is the closure.)

## Configuration

Two configuration layers:

**Global config (shared with the CLI, Linux)** — `/etc/reposync/reposync.conf`
(DEB822). This is the *same file the CLI reads*; the GUI never keeps a second
copy of it. It carries the repo control command sets: each `Repo-Type:` record
declares `Path:` / `Import-Cmd:` / `Remove-Cmd:` and the active type is picked
with `Active-Repo:` (several command sets, switchable via one config line):

```
Active-Repo: reprepro

Repo-Type: reprepro
Path: /srv/target
Import-Cmd: reprepro -b /srv/target includedeb trixie
Remove-Cmd: reprepro -b /srv/target remove trixie

Repo-Type: plain
Path: /srv/plain
Import-Cmd: cp -t /srv/plain/pool
Remove-Cmd: /usr/local/bin/plain-rm
```

The old `target.conf` is retired; `target.sources`
(`/etc/reposync/target.sources`) is now a pure apt source view (only
`Types/URIs/Suites/Components/Architectures/Trusted`, DEB822, never
`sources.list` format). Upstream sources live in
`/etc/reposync/sources.list.d/*.sources` (DEB822 only).

**Transport**: index fetches and `.deb` downloads go through **libcurl**
(linked in — no `curl`/`curl.exe` binary is assumed anywhere), so source
`URIs` may be `file://`, `http://` or `https://` — including the target
repo's own index (`update` also reads `target.sources`). HTTP is the
first-class path; HTTPS rides on OpenSSL (`libssl`/`libcrypto`, dragged in
by libcurl), which is Apache-2.0/OpenSSL licensed and stays clear of
GPL/LGPL like everything else.

**GUI session config** — written on exit to `$HOME/.config/reposync-gui.conf`
(Linux) or `%APPDATA%\reposync-gui.conf` (Windows). GUI-only preferences only
(CLI path, download dir, SSH endpoint):

```
[general]
reposync_path = reposync        # local CLI binary (Linux mode)
download_dir = .

[ssh]
enabled = 0                     # tick the SSH box in the UI (always 1 on Windows)
host = 127.0.0.1
port = 22
user = root
password =                      # empty -> try private keys
remote_reposync_path = reposync # CLI command on the remote host
remote_db_path = /var/lib/reposync/packages.db   # pulled locally after update
```

Notes:

- **Config file encoding**: written as UTF-8 without BOM. If you hand-edit it
  with Notepad, save as "UTF-8" — the parser tolerates a BOM, but value lines
  must not carry stray `\r`/`\n`.
- **SSH auth (libssh2)**: if the password field is non-empty it is used
  (password authentication). Otherwise the code tries your private keys in
  order — `~/.ssh/id_ed25519`, `id_rsa`, `id_ecdsa` on Linux, and
  `%USERDIR%\.ssh\id_ed25519`, `id_rsa`, `id_ecdsa` on Windows
  (`%USERDIR%` = `C:\Users\%USERNAME%`). Empty user defaults to `root`.
- **Remote PATH**: the SSH session is non-interactive; if `reposync` is not in
  the default PATH, set `remote_reposync_path` to its absolute path.
- **Local mode as root**: if the CLI needs `/etc/reposync` / `/var/lib/reposync`
  write access, run `sudo reposync-gui`, or point the CLI at your own paths
  with the `REPOSYNC_CONFIG_DIR` / `REPOSYNC_LIST_DIR` / `REPOSYNC_DB` /
  `REPOSYNC_TARGET` / `REPOSYNC_CONF` / `REPOSYNC_MANUAL` environment
  variables before starting the GUI (local mode inherits them).
- **Input safety**: package names and paths are validated before building the
  command line; characters outside a safe set are rejected with an error in
  the status bar.

## selftest (headless backend check)

Verifies the backend plumbing without a display — useful to debug SSH setup:

```sh
make -C gui selftest
./gui/selftest                                  # local: reposync help
./gui/selftest --ssh host port user pass "reposync update"
```

`--ssh` uses the libssh2 backend for real: TCP connect, handshake, password
auth, exec, streamed stdout/stderr, exit-code propagation. `transport_rc=0`
means the SSH round-trip itself succeeded; `exitcode` is the remote command's
exit status. A Windows `selftest.exe` can be built the same way from
`selftest.c` (link `-lssh2 -lws2_32 -lcrypt32`) to verify the WinSock+libssh2
path on the real machine.

## gui-selftest (headless GUI self-test)

`reposync-gui --gui-selftest` builds the real window, runs one **update** job
through the configured backend (same code path as pressing the button), prints
the full log buffer and the transport/exit result, then quits. The program's
exit code is `0` only when the job's transport succeeded **and** the remote
command exited `0`; otherwise `1`. Run it under a display server; on Linux
headless use `xvfb-run` or the dummy driver:

```sh
xvfb-run -a ./gui/reposync-gui --gui-selftest
SDL_VIDEODRIVER=dummy ./gui/reposync-gui --gui-selftest
```

Note that on a machine without `/etc/reposync/sources.list.d` the job will
rightly fail with `reposync: cannot open config dir ...` — that is an
environment expectation, not a code failure; `transport_ok=1` is the part that
proves the transport/UI plumbing.

## License

Project code is unlicensed/private unless stated otherwise. Third-party
libraries used, and only these, are SDL3 (zlib), SDL3_ttf (zlib), libssh2
(BSD 3-clause) and SQLite3 (public domain) — none of which is GPL or LGPL,
by design.
