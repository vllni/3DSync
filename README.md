# 3DSync

Homebrew for the Nintendo 3DS/2DS family that synchronises saves and files with cloud storage.  
Supports **Dropbox** and **Google Drive** (bidirectional sync).

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

```ini
[Dropbox]
Token=<access token from configurator>
```

### Google Drive

```ini
[GoogleDrive]
ClientId=<OAuth client ID>
ClientSecret=<OAuth client secret>
RefreshToken=<refresh token from configurator>
FolderId=<optional root folder ID>
```

`FolderId` restricts all Drive operations to one folder. Leave it out to use Drive root.

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

1. The **local file** modification time (`st_mtime`).
2. The **Drive file** MD5 checksum from the Drive API.
3. The **manifest** — a local JSON file that records the mtime and MD5 from the last successful sync.

| Local changed? | Drive changed? | Action |
|:-:|:-:|---|
| No | No | Skip (up to date) |
| Yes | No | Upload local → Drive, update manifest |
| No | Yes | Download from Drive → local, update manifest |
| Yes | Yes | **Conflict** — 3DS shows a prompt: press **A** to keep the 3DS version (upload) or **B** to keep the Drive version (download) |
| Local missing | Drive exists | Download from Drive |
| Local exists | Drive missing | Upload to Drive |
| Both missing | — | Remove stale manifest entry |

"Changed" means the mtime (local) or MD5 (Drive) differs from the last manifest entry.  
On first run (no manifest entry yet), the local file is uploaded and the manifest is initialised.

### Subfolder structure on Drive

Bidirectional paths preserve the relative directory structure on Drive.  
Example: local file `/3ds/Checkpoint/saves/TitleA/001.sav` synced with remote name `Checkpoint` appears on Drive at `Checkpoint/saves/TitleA/001.sav`.

Upload-only paths still use **flat filenames** (`saves_TitleA_001.sav`) for backwards compatibility.

### Manifest file

`/3ds/3DSync/manifest.json` is a plain JSON file stored only on the SD card:

```json
{
  "/3ds/Checkpoint/saves/TitleA/001.sav": {"mtime": 1716905520, "md5": "abc123", "id": "driveFileId"},
  "/roms/nds/saves/game.sav":             {"mtime": 1716910000, "md5": "def456", "id": "anotherFileId"}
}
```

Delete this file to force a full re-upload on the next run.

---

## Clock Skew Warning

If the 3DS system clock differs from the Drive server time by more than 60 seconds, 3DSync will print a warning. Inaccurate timestamps may cause unnecessary uploads or missed downloads — keep the 3DS clock synchronised.

---

## Google Drive App Setup

1. Go to [Google Cloud Console](https://console.cloud.google.com/).
2. Create a project, enable the **Google Drive API**.
3. Create an **OAuth 2.0 Web application** client (Client ID + Client Secret).
4. Add the redirect URI: `https://vllni.github.io/3DSync/configurator.html`.
5. Use the [configurator](https://vllni.github.io/3DSync/configurator.html) to authenticate; it performs PKCE and stores the refresh token.

> **Note:** Apps in *Testing* mode issue refresh tokens that expire after **7 days**. Publish the app or add your Google account as a test user to avoid frequent re-authentication.

---

## Building

Requires [devkitPro](https://devkitpro.org/) with 3DS support.

```bash
make
```

Output: `output/3ds-arm/3DSync.cia` and `output/3ds-arm/3ds/3DSync/3DSync.3dsx`.

