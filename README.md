# 3DSync

Homebrew for the Nintendo 3DS/2DS family that synchronises saves and files with cloud storage or a server on your own network.

| Remote | Direction | Status |
|---|---|---|
| Google Drive | bidirectional | stable |
| SMB2/3 file server (NAS, Windows share) | bidirectional | 🧪 experimental |
| FTP / FTPS | bidirectional | 🧪 experimental |
| WebDAV | bidirectional | 🧪 experimental |
| Dropbox | bidirectional | 🧪 experimental |

> 🧪 **Experimental** backends work but have had far less testing against real
> servers than Google Drive. Keep a backup of your saves, and please report
> anything that goes wrong at
> [github.com/vllni/3DSync/issues](https://github.com/vllni/3DSync/issues) —
> bug reports and feedback are what move a backend off this list. 3DSync also
> prints this notice on the 3DS before syncing with an experimental remote.

Full credit to [Kyraminol](https://github.com/Kyraminol) for the original project. Extended by michvllni with Google Drive support, refresh-token auth, and full bidirectional sync.

---

## Quick Start

1. Open the [configurator](https://vllni.github.io/3DSync/configurator.html) and follow the three steps.
2. Place the downloaded `3DSync.ini` on the SD card at `/3ds/3DSync/3DSync.ini`.
3. Install `output/3ds-arm/3DSync.cia` **or** run `3DSync.3dsx` from the Homebrew Launcher.
4. Launch 3DSync. It will sync all configured paths and press **START** to exit.

---

## Configuration

The INI file lives at `/3ds/3DSync/3DSync.ini`.

### Dropbox

🧪 Experimental.

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
| Yes | Yes | **Conflict** — 3DS shows a prompt: press **A** to keep the 3DS version (upload) or **B** to keep the remote version (download) |
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

## Google Drive App Setup

1. Go to [Google Cloud Console](https://console.cloud.google.com/).
2. Create a project, enable the **Google Drive API**.
3. Create an **OAuth 2.0 Web application** client (Client ID + Client Secret).
4. Add the redirect URI: `https://vllni.github.io/3DSync/configurator.html`.
5. Use the [configurator](https://vllni.github.io/3DSync/configurator.html) to authenticate; it performs PKCE and stores the refresh token.

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

