# Agent context for 3DSync

> **Keep this file current.** It is the shared context for every agent session in
> this repo. When a change makes something here wrong or incomplete — a module
> gains a capability, a convention changes, a build flag or file moves — update
> the affected section in the *same* commit as the code change. A stale note here
> costs a later session more time than writing it did.

## What this repo is

Nintendo 3DS/2DS homebrew app (`.cia` / `.3dsx`) that syncs files between the SD card and a remote: Google Drive, Dropbox, an SMB2/3 file server, FTP/FTPS or WebDAV. Written in C++11, built with devkitPro/devkitARM.

Companion static web configurator at `docs/` (GitHub Pages at `https://3dsync.villani-ulm.de/`) generates the INI config file via OAuth and a stepper UI.

---

## Repo layout

```
source/             C++ application source
  main.cpp          Entry point, INI parsing, provider construction, 3DS UI
  sync/
    syncengine.cpp/h  The decision table: what to upload, download, prompt
                    about or skip.  Free of 3DS APIs — see "Tests".
  modules/
    syncprovider.h  SyncProvider interface + RemoteFileInfo — the sync engine's
                    only view of a remote.  Add a backend by implementing it.
    googledrive.cpp/h  Google Drive: OAuth refresh, MD5 tags, flat legacy upload
    smbremote.cpp/h    SMB2/3 via libsmb2 (NAS, Windows share)
    ftpremote.cpp/h    FTP/FTPS via libcurl
    webdavremote.cpp/h WebDAV via libcurl (PROPFIND/MKCOL/PUT/MOVE)
    dropbox.cpp/h   Dropbox: content_hash tags, PKCE refresh-token auth
    remoteparse.cpp/h  Response parsing and URL building for the remotes,
                    kept free of libcurl/libsmb2 so it is directly testable
    manifest.cpp/h  Local JSON manifest tracking sync state
  utils/
    curl.cpp/h      libcurl wrapper (methods, upload/download streaming, FTP
                    options, wildcard listing, header capture)
    json.cpp/h      JSON member lookups, escaping, array splitting
    xmlutil.cpp/h   WebDAV multistatus reading
    urlutil.cpp/h   Percent-encoding
    pathutil.cpp/h  Remote path normalisation, in-flight transfer names
    timeutil.cpp/h  Server clock parsing (RFC 3339 and RFC 7231)
    fsutil.cpp/h    mkdirs, temp-file + atomic-replace helpers
    hash.cpp/h      MD5 and the Dropbox content hash
    console.h       CONSOLE_* colours, from <3ds.h> or defined for the host
  libs/inih/        INI parser (inih + INIReader)
tests/              Host unit tests (see "Tests")
docs/               Configurator static site
  index.html        Three-step stepper UI (auth → paths → download)
  static/js/index.js  All UI logic (PKCE OAuth, path config, INI generation)
output/             Build artefacts (.cia, .3dsx, .zip) — do not edit
build/              Intermediate object files — do not edit
buildtools/         devkitPro build scaffolding (git submodule) — do not edit
.github/            CI workflows, CODEOWNERS
```

---

## Build

Requires devkitPro with 3DS support (`DEVKITPRO` environment variable set), plus **libsmb2**, which devkitPro does not package. The devcontainer image cross-compiles it into `$DEVKITPRO/portlibs/3ds` (see `docker/3dsync-devcontainer.Dockerfile`, pinned to a commit). To build it by hand:

```bash
cmake -S libsmb2 -B build -DCMAKE_TOOLCHAIN_FILE=$DEVKITPRO/cmake/3DS.cmake \
  -DCMAKE_INSTALL_PREFIX=$DEVKITPRO/portlibs/3ds -DBUILD_SHARED_LIBS=OFF \
  -DENABLE_EXAMPLES=OFF -DENABLE_LIBKRB5=OFF -DENABLE_GSSAPI=OFF -DENABLE_LIBDCERPC=OFF
cmake --build build && cmake --install build
```

Because the published CI image is only rebuilt on pushes to master, `.github/workflows/pr.yml` builds it locally when a PR touches `docker/*Dockerfile`.

```bash
make          # produces output/3ds-arm/3DSync.cia and .3dsx
```

The Makefile auto-discovers all `.cpp` files under `source/`. Adding a new `.cpp` file there is sufficient — no Makefile edits needed.

`make` needs the `buildtools` submodule checked out (`git submodule update --init`); without it the build stops at `buildtools/make_base: No such file or directory`. When a full build is not available, type-check a translation unit directly:

```bash
arm-none-eabi-g++ -fsyntax-only -std=gnu++11 -Wall \
  -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -D__3DS__ -DARM11 \
  -DINI_MAX_LINE=1024 -DVERSION_STRING='"x"' -DREVISION_STRING='"y"' \
  -I$DEVKITPRO/libctru/include -I$DEVKITPRO/portlibs/3ds/include \
  -I$DEVKITPRO/portlibs/armv6k/include source/main.cpp
```

Compile-time configuration of vendored libraries belongs in `BUILD_FLAGS` in the Makefile, not in the library sources. `INI_MAX_LINE=1024` is set there (inih defaults to 200, which truncates long `Paths=` lines); leave `source/libs/inih/` pristine.

---

## Key C++ conventions

- **C++11**, no exceptions, no RTTI.
- Logic worth testing goes in a unit that does not include `<3ds.h>`, libcurl or libsmb2, and gets a test. The providers should stay thin: talk to the network, hand the response to a parser.
- All HTTP goes through the `Curl` wrapper (`source/utils/curl.h`). Never call libcurl directly.
- `Curl::perform()` returns 0 on network success. **HTTP status is not checked by `perform()`** — callers must call `getStatusCode()`. `CURLOPT_FAILONERROR` is intentionally disabled so error response bodies are captured.
- Every provider owns a `_performWithRetry()` that maps transport errors to *transient* (retry with back-off), *fatal* (`_fatalError`, stop the run) or *per-file* (log and skip). Never call `_curl.perform()` directly from a provider method — the retry helper is also where the server `Date` header is captured.
- libcurl options are sticky on a handle. Providers that issue more than one shape of request call `Curl::reset()` first (via their own `_prepare()`); otherwise `NOBODY`, `UPLOAD`, `WILDCARDMATCH` or a custom method leaks into the next call.
- Fatal Drive errors (401 / unrecoverable 403) set `_fatalError = true` and cause all subsequent Drive calls to no-op. Check `hasFatalError()` in callers to break out of sync loops early.
- File downloads use a temp file (`localPath + ".3dstmp"`) + atomic rename, via `openTempFor()` / `replaceLocalFile()` in `utils/fsutil.h`. On FAT (3DS SD), `rename()` cannot overwrite an existing file — the existing file is first renamed to `.3dsbak`, then the temp is renamed in, then the backup is removed. Restore the backup on failure.
- Uploads are equally guarded: write to `<remote>.3dstmp`, then swap it into place (WebDAV `MOVE` with `Overwrite: T`, FTP `RNFR`/`RNTO` post-quote, SMB unlink-then-rename because libsmb2 sends `ReplaceIfExists=0`). Listings skip `.3dstmp` leftovers.
- `performSync()` in `main.cpp` is provider-neutral: it talks only to `SyncProvider`. It returns `bool` — `false` means cancellation was requested or a fatal error occurred, and the sync loop must stop. Its `uploadOnly` argument mirrors local → remote: never download, never prompt, leave remote-only files alone.
- Change detection compares the local mtime **and** size against the manifest, plus the remote's `tag`. `SyncProvider::localTag()` is the content-based fallback for stale mtimes and is only meaningful where the remote's tag is derivable from file contents (Drive's MD5); it returns "" elsewhere.
- Manifest keys are `SyncProvider::manifestPrefix() + "|" + localPath`. Changing a provider's prefix orphans every existing entry for that remote, which makes the next sync treat every file as unseen — so keep them stable.
- Conflict resolution in `performSync()` calls `waitForConflictKey()` which returns a `ConflictChoice` enum: `CONFLICT_KEEP_LOCAL` (A), `CONFLICT_KEEP_REMOTE` (B), `CONFLICT_SKIP` (X), `CONFLICT_CANCEL` (START).
- The manifest (`/3ds/3DSync/manifest.json`) is a hand-parsed JSON file. Use `Manifest::set/get/has/remove` — never write JSON by hand elsewhere.
- `svcSleepThread(nanoseconds)` is the 3DS sleep call (from `<3ds.h>`). Use it for rate-limit back-offs.
- `printf` output goes to the 3DS top-screen console. Use `CONSOLE_RED` / `CONSOLE_RESET` (from `<3ds.h>`) for error messages.
- The console is **50 columns** wide and wraps mid-word. Keep any line of output — key prompts especially — under that.
- Keep sync output quiet: print per-entry headers, the experimental notice, errors, interactive prompts and the closing `--- Sync Summary ---`. Do **not** add a line per file examined or per file transferred; the summary already lists every changed file, and per-file chatter scrolls the interesting parts off-screen.
- `Curl::setReadData(FILE *, size)` streams a request body from a file. Pass the real byte count: without it libcurl sends `Transfer-Encoding: chunked`, which the Dropbox content endpoints reject.

---

## Tests

```bash
make -C tests            # build and run
make -C tests F=engine   # only tests whose "suite.name" contains "engine"
```

They build for the **host**, not the 3DS, and run in the `Unit Tests` job of `pr.yml`. Nothing here needs hardware or an emulator, which is only possible because the logic under test is kept free of libctru, libcurl and libsmb2 — keep it that way when adding code:

- `sync/syncengine.cpp` talks to `SyncProvider` and `SyncUi` and nothing else. Cancellation and the conflict prompt arrive through `SyncUi` precisely so the decision table can be driven by a scripted stand-in; do not reach for `hidKeysDown()` or `aptMainLoop()` in there.
- Response parsing lives in `modules/remoteparse.cpp`, so a provider's `list()` is a request plus a call into a testable function. New protocol parsing belongs there, not inline in the provider.
- `utils/console.h` supplies the `CONSOLE_*` macros off-console, so host-portable code must include it instead of `<3ds.h>`.
- The hash tests need mbedtls on the host (`libmbedtls-dev`). Without it the suite still builds and runs, and the runner prints what was left out — deliberately loud rather than a silent pass.

The framework is `tests/framework.h`, about eighty lines: the build image has no gtest and vendoring one for this is more dependency than it is worth. `TEST(suite, name)` registers a case; `CHECK`/`CHECK_EQ`/`CHECK_STR_EQ` record a failure and keep going, so one broken case reports every mismatch rather than only the first.

**Mocking.** `tests/mocks.h` has `MockRemote` (a `SyncProvider` holding its files in memory, with scripted failures and recorded calls), `MockUi` (scripted conflict answers and cancellation), and `TempDir` (a real scratch directory for the local side). Mocking at `SyncProvider` is deliberate: it is the one seam every backend shares, so those tests cover the engine for all five remotes at once. An HTTP-level mock could not — SMB does not speak HTTP. Per-remote behaviour is covered instead by feeding recorded server responses through `remoteparse`.

What is *not* covered, and would need hardware or a much larger seam: the request-building paths inside each provider (libcurl option wiring, libsmb2 calls), retry and back-off behaviour, and the OAuth flows. A mockable transport interface would reach the first of those; it was left out because it means rewriting request code in four providers that have not yet been validated against real servers.

---

## Remotes

Everything the engine needs from a backend is in `SyncProvider` (`source/modules/syncprovider.h`): `connect`, `ensureRoot`, `list`, `download`, `upload`, `hasFatalError`, and the optional `localTag` / `serverTime` / `legacyUpload` / `isExperimental`. A new protocol means one new module and three lines in `runSync()`; `performSync()` should not need to change.

`isExperimental()` **defaults to true**, so a newly added backend warns until someone deliberately marks it stable. Only `GoogleDrive` overrides it to false. When the flag is set, `syncProvider()` prints a notice on the 3DS naming the backend and the issue-tracker URL (`ISSUES_URL` in `main.cpp`) before the first transfer.

Flipping a backend to stable is four edits, and all four should move together: the `isExperimental()` override, the status table at the top of `README.md`, the section note under its INI example there, and the badge on `docs/index.html` (plus `docs/configurator.html` for a backend the configurator can write).

Constraints that shaped the current set — worth knowing before proposing a fourth:

- **No SFTP.** devkitPro's libcurl 8.4.0 is built without libssh2 (`libcurl_la-libssh2.o` is empty; `curl-config --protocols` lists no SCP/SFTP). FTPS is the encrypted option.
- **curl's SMB is useless for sync.** It is SMB1-only (`NT LM 0.12`) and cannot list a directory, hence libsmb2 instead.
- **libsmb2's sync API blocks on `poll()`**, which libctru provides (`libctru/include/poll.h`). Include `smb2/smb2.h` *before* `smb2/libsmb2.h` — the latter uses macros the former defines.
- **`curl_fileinfo::time` is documented "always zero"**, so FTP timestamps come from a per-file `MDTM`/`SIZE` request after the wildcard listing. That is one extra round trip per file; do not "optimise" it away by trusting the listing's time.
- **Only Drive and Dropbox offer a content hash** (MD5 and `content_hash`), so only they can implement `localTag()`. SMB, FTP and WebDAV tag with size + mtime (or an ETag), which makes a modification preserving both invisible to them.
- Credentials for SMB/FTP/WebDAV sit in plaintext in `3DSync.ini`, as the INI is the only configuration channel. Do not log them.

---

## Dropbox module (`source/modules/dropbox.cpp`)

A full `SyncProvider`: `files/list_folder` (+ `list_folder/continue`), `files/download`, `files/upload`, and `content_hash` as the change tag.

Things here that are easy to get wrong:

- **`content_hash` is not an MD5.** It is SHA-256 over the concatenated SHA-256 digests of each 4 MiB block; `computeDropboxHash()` in `utils/fsutil.cpp` reproduces it, which is what lets `localTag()` catch a save whose mtime never moves. An empty file hashes to SHA-256 of no input at all — do not "fix" that by hashing a zero-length block.
- **Downloads address `rev:<rev>`, not the path**, so the bytes that arrive are the revision `list()` reported even if the file changes mid-sync. `RemoteFileInfo::id` therefore holds `"rev:…"`; `upload()` rebuilds the real path from `root + relPath` instead of reading it back.
- Uploads use `"mode":"overwrite"`. The API default `"add"` does **not** overwrite — it silently renames the upload to `name (1).ext`, which breaks sync entirely.
- **A single-request upload is capped at 150 MB.** Larger files need the `upload_session` endpoints, which nothing on an SD card should hit; the module refuses them with a clear message rather than failing obscurely.
- `_headerJsonEscape()` — `Dropbox-API-Arg` is JSON carried in an HTTP header and must be pure ASCII, so non-ASCII path bytes are emitted as `\uXXXX`. Save folders are named after game titles, so non-ASCII paths are normal, not an edge case. `_jsonEscape()` is the laxer version for request bodies.
- `_splitEntries()` walks the `entries` array counting braces **string-aware**: a file name may contain `{` or `}`.
- **Dropbox paths are case-insensitive.** `list()` matches the root with `strncasecmp` and keeps the server's spelling for the remainder. A folder stored under a different case than the local one would produce a `relPath` that does not match, so the two sides would be treated as separate files.
- `validateToken()` — `POST /2/check/user` (needs no scopes) before any transfer, so a dead token gives one clear message instead of one per file.
- `_performWithRetry()` / `_rpc()`: back-off on network errors, on 429 (honouring `Retry-After`) and on 5xx; 401/403 set `_fatalError`; 400/409 are per-call errors that log the response body and continue. Do not call `_curl.perform()` directly.

**Auth.** `RefreshToken=` + `AppKey=` is the supported setup: Dropbox access tokens expire after about four hours, so `connect()` exchanges the refresh token for one on every run. `AppSecret=` is only sent for a confidential (non-PKCE) app. A bare `Token=` is still accepted for configs written before this flow existed, and `validateToken()` says what to do when such a token has expired. The configurator uses PKCE with `token_access_type=offline`; both providers now return `?code=&state=`, and the stored `state` decides which one is answering.

---

## INI format (on SD card)

```ini
[GoogleDrive]
ClientId=...
ClientSecret=...
RefreshToken=...
FolderId=...          ; optional

[Dropbox]
AppKey=...            ; app key from the configurator
RefreshToken=...      ; PKCE refresh token; Token= (short-lived) still accepted
AppSecret=            ; only for a confidential (non-PKCE) app
Path=                 ; optional folder inside the account

[SMB]                 ; SMB2/3 file server
Server=192.168.1.10
Share=3ds
User=...
Password=...
Domain=               ; optional
Path=                 ; optional folder inside the share

[FTP]
Host=192.168.1.10
Port=                 ; optional, 21 / 990 for TLS=implicit
User=...
Password=...
Path=                 ; optional base directory
TLS=try               ; none | try | require | implicit
Mode=passive          ; passive | active

[WebDAV]
Url=https://host/remote.php/dav/files/user/
User=...
Password=...

[Paths]               ; bidirectional, recursive
RemoteName=/LocalPath

[ShallowPaths]        ; bidirectional, non-recursive
[UploadPaths]         ; upload-only, recursive
[UploadShallowPaths]  ; upload-only, non-recursive
```

---

## Configurator (docs/)

- Pure static site — no server. Credentials are entered by the user (their own Google Cloud project). The client secret lives in `localStorage` only long enough to survive the OAuth redirect, and must be cleared from `localStorage` after `exchangeGoogleCode` succeeds.
- OAuth flow: PKCE (S256), authorization code, refresh token stored in INI. Redirect URI is `https://3dsync.villani-ulm.de/`.
- All `target="_blank"` links must include `rel="noopener noreferrer"`.
- INI generation is in `getConfigString()` in `index.js`. It reads `localStorage` for provider, tokens, and folder ID.

---

## Licensing

3DSync's own code is MIT. **libsmb2 is LGPL-2.1** and is statically linked, so the released binaries are a combined work. The relinking requirement is met by keeping the source public and the libsmb2 commit pinned in the Dockerfile — do not vendor a modified copy without recording the changes, and keep the note in README.md accurate.

---

## Branch / PR conventions

- Feature branches: `feat/...`, bug fixes: `fix/...` or `fiX/...`
- PRs target `vllni/3DSync:master` (the fork), **not** `Kyraminol/3DSync:master` (the upstream).
- Always run `make` and `make -C tests` before committing C++ changes.
- Commit messages: short imperative subject, blank line, then bullet-point body describing *why*.
- `.github/CODEOWNERS` assigns `@vllni` as the default reviewer for the whole repo.
- Never reuse code from a closed PR whose author has withdrawn permission for it. Implementing the same fix independently is fine; copying the commits is not.
