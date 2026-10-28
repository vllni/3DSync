# 3DSync

Homebrew for the Nintendo 3DS/2DS family that synchronises saves and files with cloud storage or a server on your own network.

| Remote | Direction | Status |
|---|---|---|
| Google Drive | bidirectional | stable |
| Dropbox | bidirectional | stable |
| SMB2/3 file server (NAS, Windows share) | bidirectional | 🧪 experimental |
| FTP / FTPS | bidirectional | 🧪 experimental |
| WebDAV | bidirectional | 🧪 experimental |

> 🧪 **Experimental** backends work but have had far less testing against real
> servers than Google Drive. Keep a backup of your saves, and please report
> anything that goes wrong at
> [github.com/vllni/3DSync/issues](https://github.com/vllni/3DSync/issues) —
> bug reports and feedback are what move a backend off this list. 3DSync also
> prints this notice on the 3DS before syncing with an experimental remote.

Full credit to [Kyraminol](https://github.com/Kyraminol) for the original project. Extended by michvllni with Google Drive support, refresh-token auth, and full bidirectional sync.

---

## Quick Start

1. Open the [configurator](https://3dsync.vllni.dev/configurator.html) and follow the three steps.
2. Place the downloaded `3DSync.ini` on the SD card at `/3ds/3DSync/3DSync.ini`.
3. [Install 3DSync](#installation), most easily through Universal-Updater.
4. Launch 3DSync and press **A** (or tap **Sync now**) to sync all configured
   paths, or **Y** to sync with [debug output](#debug-mode). **START** exits.

### Controls

The top screen is the log; the bottom screen shows what 3DSync is doing and the
choices open right now. Every touch button is also a physical button, and its
label says which, so either works:

| Where | Buttons |
|---|---|
| Menu | **A** sync, **Y** sync with debug output, **START** exit |
| During a sync | **START** stops after the current file |
| Conflict | **A** keep the 3DS version, **B** keep the remote one, **X** skip, **L** apply the answer to every remaining conflict, **START** stop |
| End of a debug run | **A** save the log, **B** discard it |

The log can be scrolled at any time, including while a sync is running:
**D-pad ↑/↓** one line (hold to keep going), **D-pad ←/→** one page, the
**Circle Pad**, or the ▲/▼ buttons and the scroll bar on the right of the
touch screen. While you are scrolled up, new output does not move the text;
scroll back to the bottom and the log follows it again.

---

## Installation

All three ways need a 3DS running custom firmware with the Homebrew Launcher
or FBI; see [3ds.hacks.guide](https://3ds.hacks.guide/) if yours does not.

### Universal-Updater (recommended)

<img src="https://3dsync.vllni.dev/unistore/3dsync-unistore-QR.png" alt="QR code of the 3DSync UniStore" width="200" align="right">

1. In [Universal-Updater](https://github.com/Universal-Team/Universal-Updater),
   open **Settings → Select a UniStore → +**.
2. Scan the QR code, or type the store URL:
   `https://3dsync.vllni.dev/unistore/3dsync.unistore`
3. Select the 3DSync store, open the 3DSync entry and pick a download:
   - **3DSync.cia** installs it to the HOME Menu.
   - **3DSync.3dsx** puts it in the Homebrew Launcher.

New releases show up in the store by themselves, and the entry is flagged
when an update is available. Earlier versions stay listed as
`[v0.6.0] 3DSync.cia` and so on, should you need to go back.

### FBI

Every [release](https://github.com/vllni/3DSync/releases) carries a QR code
for its `.cia`. In FBI, choose **Remote Install → Scan QR Code** and scan it.

### By hand

Download from the [latest release](https://github.com/vllni/3DSync/releases/latest):

- `3DSync-<version>.cia` — copy it to the SD card and install it with FBI.
- `3DSync-<version>.3dsx` — copy it to `/3ds/3DSync/3DSync.3dsx` and launch it
  from the Homebrew Launcher.

The `.zip` holds both, plus a `.3ds` for flashcarts.

Updating or reinstalling keeps your settings: `3DSync.ini`, the sync manifest
and the debug logs live in `/3ds/3DSync/` on the SD card, not inside the app.

---

## Configuration

The INI file lives at `/3ds/3DSync/3DSync.ini`.

### Dropbox

```ini
[Dropbox]
AppKey=<app key from configurator>
RefreshToken=<refresh token from configurator>
Path=                     ; optional folder inside the Dropbox account
```

Configs with a bare `Token=` still work, but Dropbox access tokens expire after
about four hours — re-run the configurator to get a refresh token instead.
`AppSecret=` is only needed if you bring your own non-PKCE ("confidential")
Dropbox app.

### Google Drive

```ini
[GoogleDrive]
ClientId=<OAuth client ID>
ClientSecret=<OAuth client secret>
RefreshToken=<refresh token from configurator>
FolderId=<optional root folder ID>
```

`FolderId` restricts all Drive operations to one folder. Leave it out to use Drive root.

### SMB file server (NAS, Windows share)

🧪 Experimental.

```ini
[SMB]
Server=192.168.1.10       ; IP address or hostname
Share=3ds                 ; share name, without slashes
User=<username>
Password=<password>
Domain=                   ; optional, for a Windows domain
Path=                     ; optional folder inside the share
```

Speaks SMB2/SMB3 — the dialects modern NAS boxes and Windows accept by default.
SMB1 is not supported and does not need to be re-enabled.

### FTP / FTPS

🧪 Experimental.

```ini
[FTP]
Host=192.168.1.10
Port=                     ; optional, defaults to 21 (990 for TLS=implicit)
User=<username>
Password=<password>
Path=                     ; optional base directory
TLS=try                   ; none | try | require | implicit
Mode=passive              ; passive | active
```

`TLS=try` (the default) encrypts the session when the server offers `AUTH TLS`
and falls back to plaintext when it does not. Use `require` to refuse an
unencrypted session, or `implicit` for the older `ftps://` on port 990.

An **anonymous** login (no `User=` line, or `User=anonymous` / `User=ftp`)
with `TLS=try` also falls back to plaintext when the server accepts TLS but
then refuses the login over it — what a stock vsftpd does
(`530 Anonymous sessions may not use encryption`). 3DSync reconnects once
without TLS and stays that way for the rest of the run; a [debug
run](#debug-mode) says so. A **named user** never falls back, since that would
send the password unencrypted right after the server refused it over TLS: the
login fails, and if the server needs plaintext logins, set `TLS=none`.

SFTP is **not** supported: the 3DS build of libcurl has no libssh2.

### WebDAV

🧪 Experimental.

```ini
[WebDAV]
Url=https://nas.example/remote.php/dav/files/username/
User=<username>
Password=<app password>
```

Works with Nextcloud/ownCloud and any NAS with WebDAV enabled. Use an app
password rather than your account password where the server offers one.

Configure as many remotes as you like — each configured section is synced in
turn against the same set of paths, and each keeps its own state in the
manifest.

### Sealed credentials

`3DSync.ini` is the only way to give 3DSync your passwords and tokens, and it
lives on an SD card that gets pulled, copied to PCs and passed around. So the
first time 3DSync starts with a new file, it **seals** every credential in it
to that console:

```ini
[SMB]
Server=192.168.1.10
Password=enc:v1:Wm9vbGFuZCBpcyBub3QgYSByZWFsIHBhc3N3b3Jk…
```

The value is encrypted (AES-256-GCM) with a key derived from a secret unique
to the console, which is not on the SD card. On that 3DS, nothing changes. On
its own — a copy of the card, a file attached to a bug report — the value is
useless. The rest of the file stays readable and editable.

- **Changing a password:** type the new one over the sealed value in plain
  text. It is sealed on the next start.
- **Another console, an emulator, or after a system transfer:** the sealed
  values cannot be opened there. 3DSync says so and skips that remote rather
  than log in with garbage; paste the plain values back in, or make a new file
  with the configurator.
- **Going back to a version before 0.7.0** after sealing: older versions do not
  understand sealed values and fail to log in. Make a new file with the
  configurator.
- **Sharing one file between several consoles:** turn sealing off.

  ```ini
  [Security]
  SealCredentials=false
  ```

What sealing does not do: anyone who can run homebrew on *your* console can
derive the same key. It protects the card, not the console. What limits the
damage either way is giving 3DSync as little as possible — a NAS or FTP user
of its own that reaches only the save folder, a WebDAV app password, a Dropbox
app with *App folder* access — and the configurator suggests how for each.

---

## Sync Paths

Four INI sections control which local paths to sync and how:

| Section              | Direction        | Subdirs |
|----------------------|------------------|---------|
| `[Paths]`            | Bidirectional    | Yes     |
| `[ShallowPaths]`     | Bidirectional    | No      |
| `[UploadPaths]`      | Upload only      | Yes     |
| `[UploadShallowPaths]` | Upload only    | No      |

Each entry has the form `RemoteName=LocalPath`:

```ini
[Paths]
Checkpoint=/3ds/Checkpoint/saves
roms/nds/saves=/roms/nds/saves

[UploadPaths]
Backup=/3ds/MyGame
```

- **RemoteName** is the Drive folder path (use `/` for nesting, e.g. `roms/nds`).
- **LocalPath** is the absolute path on the SD card.
- Bidirectional entries use a **manifest** (`/3ds/3DSync/manifest.json`) to detect which side changed.
- Upload-only entries always push the local file to Drive and never download.

### File type filter

By default every file under a sync path is synced. To sync only certain file types, list their extensions under `[Filter]`:

```ini
[Filter]
Extensions=sav, bin, dat
```

- Separate extensions with commas or spaces (not `;`, which starts a comment). `sav`, `.sav` and `*.sav` all mean the same, and case does not matter.
- A file is synced only if its name ends in one of them; a longer ending such as `tar.gz` works too. Leave `Extensions=` empty, leave it out, or write `*` to sync all files.
- The filter applies to every sync path and to both directions: a filtered-out file is neither uploaded nor downloaded, and its manifest entry is kept, so loosening the filter later picks up where it left off.
- The main menu shows the active filter next to **FILES**, and a sync prints it once before the first transfer.

---

## Bidirectional Sync Logic

Bidirectional paths (`[Paths]` / `[ShallowPaths]`) compare each file against:

1. The **local file** modification time (`st_mtime`) and size.
2. The remote file's **change tag** — whatever the remote can prove a change with:

   | Remote | Change tag |
   |---|---|
   | Google Drive | MD5 checksum from the Drive API |
   | Dropbox | `content_hash` (SHA-256 over per-4 MiB-block SHA-256) |
   | WebDAV | `ETag`, falling back to size + `Last-Modified` |
   | SMB | size + modification time |
   | FTP | size + `MDTM` |

3. The **manifest** — a local JSON file that records the local mtime/size and the remote tag from the last successful sync.

| Local changed? | Remote changed? | Action |
|:-:|:-:|---|
| No | No | Skip (up to date) |
| Yes | No | Upload local → remote, update manifest |
| No | Yes | Download from remote → local, update manifest |
| Yes | Yes | **Conflict** — 3DS shows a prompt: press **A** to keep the 3DS version (upload) or **B** to keep the remote version (download); see [Controls](#controls) |
| Local missing | Remote exists | Download from remote |
| Local exists | Remote missing | Upload to remote |
| Both missing | — | Remove stale manifest entry |

"Changed" means the local mtime/size, or the remote tag, differs from the last manifest entry.  
On first run (no manifest entry yet), the local file is uploaded and the manifest is initialised.

Google Drive and Dropbox can verify a local file by content — their tags are an
MD5 and a Dropbox content hash respectively, both computable on the 3DS — so a
save whose mtime never moves (some emulators never update it) is still detected.
On SMB, FTP and WebDAV an unchanged mtime *and* size is taken as unchanged.

### Subfolder structure on the remote

Bidirectional paths preserve the relative directory structure.  
Example: local file `/3ds/Checkpoint/saves/TitleA/001.sav` synced with remote name `Checkpoint` appears at `Checkpoint/saves/TitleA/001.sav`.

Upload-only paths on **Google Drive** still use **flat filenames**
(`saves_TitleA_001.sav`) for backwards compatibility. On the other remotes,
Dropbox included, they mirror the local structure, skipping files that have not
changed.

### Manifest file

`/3ds/3DSync/manifest.json` is a plain JSON file stored only on the SD card:

```json
{
  "drive|/3ds/Checkpoint/saves/TitleA/001.sav": {"mtime": 1716905520, "size": 512, "tag": "abc123", "id": "driveFileId"},
  "smb://192.168.1.10/3ds|/roms/nds/saves/game.sav": {"mtime": 1716910000, "size": 32768, "tag": "32768:1716910000", "id": "sync/roms/game.sav"}
}
```

Each key is the remote's identifier and the full local path, so several remotes
can sync the same folder without overwriting each other's state. Manifests
written by older versions (bare local paths, an `md5` field) are read and
upgraded automatically.

Delete this file to force a full re-upload on the next run.

---

## Clock Skew Warning

If the 3DS system clock differs from the remote's clock by more than 60 seconds, 3DSync will print a warning. Inaccurate timestamps may cause unnecessary uploads or missed downloads — keep the 3DS clock synchronised.

---

## Debug Mode

A normal sync is quiet on purpose: it prints one header per configured path,
errors that stop something, and the closing summary. That means a good deal is
handled without a word — a folder that does not exist yet is treated as empty, a
file the remote refuses is skipped, an unreadable timestamp is read as
unchanged. When a sync does something you do not expect, run it again with **Y**
instead of **A**.

Debug mode prints:

- every HTTP status, the URL behind it, and the response body of anything that
  was not a success — including the API error that a failed download writes into
  the temp file, which a normal run deletes unread
- libcurl's connection and header trace, and the socket error behind a
  "Network error"
- what `3DSync.ini` actually parsed to, including the line number of a syntax
  error that was skipped
- the per-file decision: whether each side exists, whether the manifest knew
  about it, which side changed, and what was done about it
- the failures the local side hides: a `mkdir` the SD card refused, a manifest
  that was truncated by an interrupted write, a hash that could not be computed

Passwords, tokens and OAuth secrets are replaced with `<redacted>`, so the
output is safe to attach to a [bug report](https://github.com/vllni/3DSync/issues).
The rest is not filtered — a debug run prints one line per file examined. The
log on the top screen can be scrolled back while the run is going (see
[Controls](#controls)), but for a bug report a file beats a photo.

So a debug run does not leave it at the screen: when the sync ends, 3DSync
offers to write everything it printed to the SD card as
`/3ds/3DSync/logs/debug-<date>-<time>.log`. Press **A** to save it or **B** to
discard it. That file is the whole run, colour codes removed and secrets masked
— attach it to the issue instead of photographing the screen. A very long run
keeps its beginning and its end and notes in the middle how much was dropped.

The log holds the same text the screen did, which includes the local paths you
sync. Worth a look before sharing it, and it stays on your SD card until you
delete it.

The version the app is running is on the lower screen, and belongs in any
report.

---

## Google Drive App Setup

1. Go to [Google Cloud Console](https://console.cloud.google.com/).
2. Create a project, enable the **Google Drive API**.
3. Create an **OAuth 2.0 Web application** client (Client ID + Client Secret).
4. Add the redirect URI: `https://3dsync.vllni.dev/configurator.html`.
5. Use the [configurator](https://3dsync.vllni.dev/configurator.html) to authenticate; it performs PKCE and stores the refresh token.

> **Note:** Apps in *Testing* mode issue refresh tokens that expire after **7 days**. Publish the app or add your Google account as a test user to avoid frequent re-authentication.

---

## Third-party components

SMB support is provided by [libsmb2](https://github.com/sahlberg/libsmb2), which
is licensed under the **LGPL-2.1**. It is statically linked into the released
`.cia` / `.3dsx`, so those binaries are a combined work: the LGPL's relinking
requirement is met by this repository — the full source and the pinned libsmb2
commit (see `docker/3dsync-devcontainer.Dockerfile`) are public, and the build is
reproducible with `make`. 3DSync's own code remains MIT.

---

## Building

Requires [devkitPro](https://devkitpro.org/) with 3DS support, plus
[libsmb2](https://github.com/sahlberg/libsmb2) for the SMB backend — the
devcontainer image builds it for you.

```bash
make
```

Output: `output/3ds-arm/3DSync.cia` and `output/3ds-arm/3ds/3DSync/3DSync.3dsx`.

The version shown on the lower screen comes from `source/version.h`, and the
Makefile reads the CIA metadata version from the same line — so a build from a
tarball or a shallow clone reports the same version a tagged release does. A
release whose tag disagrees with that header fails in CI before it is built.

## Tests

```bash
make -C tests
```

The unit tests build for the machine you run them on, not for the 3DS, and cover
the parts that decide what a sync does: the upload/download/conflict decision
table, manifest persistence and migration, content hashing, and each remote's
response parsing driven by recorded server responses. They run on every pull
request. `libmbedtls-dev` enables the hashing tests; without it the rest still
run and the runner says what it skipped.

