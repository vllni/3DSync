# Test servers for the SMB, FTP and WebDAV remotes

`compose.yaml` runs one of each server 3DSync talks to, on the host network,
so a 3DS (or Azahar) on the same LAN reaches them directly at this machine's
address — no NAT between the 3DS and passive or active FTP.

| Service | Port(s) | What it covers |
|---|---|---|
| `ftp` | 2121, passive 30000–30009 | Explicit FTPS (`AUTH TLS` offered, not forced): `TLS=none`, `try`, `require`; passive and active |
| `ftps-implicit` | 2990, passive 30010–30019 | `TLS=implicit` |
| `ftp-plain` | 2122, passive 30020–30029 | No `AUTH TLS` at all: `TLS=try` must fall back, `TLS=require` must refuse |
| `ftp-anon` | 2123, passive 30030–30039 | Anonymous only, `AUTH TLS` offered. Read-only root, writable `pub/` |
| `ftp-anon-stock` | 2124, passive 30040–30049 | Same, but with vsftpd's default of refusing TLS for anonymous logins: `AUTH TLS` is accepted, then `USER anonymous` gets 530. Exercises 3DSync's plaintext fallback |
| `smb` | 1445 | Samba, SMB2 minimum. Shares `3ds`, `3ds-encrypted` (SMB3 encryption required) and `3ds-public` (guest, no login) |
| `webdav` | 8080 (HTTP), 8443 (HTTPS) | Apache mod_dav. `/dav/` allows `Depth: infinity`; `/dav-nodepth/` refuses it with 403, like stock Apache; `/dav-anon/` needs no login |

Everything except the anonymous servers uses user `3dsync`, password
`3dsync` (change both in `.env`).
TLS certificates are self-signed, generated on first start, and name
`HOST_IP` (and `localhost`) as subject alternative names. 3DSync checks
certificates, so every FTPS and HTTPS case here needs one of three lines in
its section, the configurator's *Server certificate* choice: `PinnedKey=`
(the server's key), `CACert=` (the server's certificate, which as a
self-signed one is its own CA), or `VerifyTLS=false`. Without any of them the
remote stops with "the server's certificate could not be verified" and shows
the `PinnedKey=` line to add — see "Server certificates" below.

## Starting

```sh
cp .env.example .env              # then set HOST_IP to this machine's LAN address
podman compose up -d --build      # or docker compose
./smoke-test.sh                   # every line should read "ok"
```

All ports are above 1024, so rootless podman binds them without a sysctl.
They are not the protocol defaults, so `3DSync.ini` names each one (below).
SMB has no `Port=` setting; libsmb2 takes the port from `Server=host:port`
instead (checked against the libsmb2 commit 3DSync pins).

Each port can be moved in `.env`, and the service listens on it directly.
Nothing else on the host may be using them. If the 3DS cannot connect, check
the host firewall lets in 1445, 2121–2124, 2990, 8080, 8443 and
30000–30049/tcp.

Files live in named volumes and survive `podman compose down`;
`podman compose down -v` wipes them. To look at or change what the 3DS
uploaded, use any client against the servers, or e.g.
`podman compose exec smb ls -lR /srv/smb`.

## Test matrix for the configurator

The [configurator](https://3dsync.vllni.dev/configurator.html) writes
one remote per file, so each row is one run through it. Replace
`192.168.1.10` with `HOST_IP` and leave every field not listed empty. An
empty User leaves the `User=` key out, which is what the anonymous rows rely
on.

### SMB

Server keeps the `:port` as typed; libsmb2 reads it from there.

| Test | Server | Share | User | Password |
|---|---|---|---|---|
| Logged in | `192.168.1.10:1445` | `3ds` | `3dsync` | `3dsync` |
| Encryption required | `192.168.1.10:1445` | `3ds-encrypted` | `3dsync` | `3dsync` |
| Guest | `192.168.1.10:1445` | `3ds-public` | *(empty)* | *(empty)* |

### FTP

Host is `192.168.1.10` throughout. Base directory `pub` is needed on 2123
and 2124, where the root is read-only.

| Test | Port | User / Password | Base directory | Encryption | Mode | Expected |
|---|---|---|---|---|---|---|
| TLS optional | `2121` | `3dsync` / `3dsync` | | Use TLS when offered | Passive | works, encrypted |
| TLS required | `2121` | `3dsync` / `3dsync` | | Require TLS | Passive | works |
| Plaintext | `2121` | `3dsync` / `3dsync` | | None | Passive | works |
| Active mode | `2121` | `3dsync` / `3dsync` | | Require TLS | Active | works |
| Implicit TLS | `2990` | `3dsync` / `3dsync` | | Implicit TLS | Passive | works |
| No TLS on server, try | `2122` | `3dsync` / `3dsync` | | Use TLS when offered | Passive | falls back to plaintext |
| No TLS on server, require | `2122` | `3dsync` / `3dsync` | | Require TLS | Passive | refused: "server does not support FTPS" |
| Anonymous | `2123` | *(empty)* | `pub` | Use TLS when offered | Passive | works, encrypted |
| Anonymous, stock vsftpd | `2124` | *(empty)* | `pub` | Use TLS when offered | Passive | falls back to plaintext and works; a debug run says so (see below) |
| Anonymous, stock vsftpd | `2124` | *(empty)* | `pub` | None | Passive | works |

### WebDAV

| Test | URL | User / Password |
|---|---|---|
| HTTP | `http://192.168.1.10:8080/dav/` | `3dsync` / `3dsync` |
| HTTPS | `https://192.168.1.10:8443/dav/` | `3dsync` / `3dsync` |
| `Depth: infinity` refused (lists folder by folder) | `https://192.168.1.10:8443/dav-nodepth/` | `3dsync` / `3dsync` |
| Anonymous | `http://192.168.1.10:8080/dav-anon/` (or `https://…:8443/dav-anon/`) | *(empty)* |

The configurator warns about `http://` URLs; for these rows that is expected.

On first start the 3DS seals every password in the file (`enc:v1:…`), and
sealed values open only on that console. Moving a section between files on
the same 3DS is fine. To use one file on several consoles or in an emulator,
add this, which is harmless here since the test passwords are not secret:

```ini
[Security]
SealCredentials=false
```

## 3DSync.ini

The same servers written by hand. Replace `192.168.1.10` with `HOST_IP`.
All of them can sit side by side and are synced together in one run. A second
remote of a type takes a label: `[SMB:enc]` next to `[SMB]` covers both SMB
shares at once (the README's "Several remotes" has `Role=` and `Only=`).

```ini
[SMB]
Server=192.168.1.10:1445
Share=3ds                 ; or 3ds-encrypted
User=3dsync
Password=3dsync

[FTP]
Host=192.168.1.10
Port=2121
User=3dsync
Password=3dsync
TLS=try                   ; none | try | require
Mode=passive              ; or active
VerifyTLS=false           ; or PinnedKey= / CACert=, see "Server certificates"
; Port=2990 with TLS=implicit for ftps-implicit
; Port=2122 for ftp-plain

[WebDAV]
Url=http://192.168.1.10:8080/dav/
; Url=https://192.168.1.10:8443/dav/
; Url=https://192.168.1.10:8443/dav-nodepth/
User=3dsync
Password=3dsync
VerifyTLS=false           ; for the https:// ones: or PinnedKey= / CACert=
```

### Server certificates

Each TLS service makes its certificate inside its container: it survives a
restart, but a recreated container (`compose down`, `up --build`) has a new
key, and a pin then has to be replaced — the renewal case, worth trying once.
To get what each line needs:

```sh
# PinnedKey= for the HTTPS WebDAV (use -starttls ftp -connect …:2121 for FTPS)
openssl s_client -connect 192.168.1.10:8443 </dev/null 2>/dev/null \
  | openssl x509 -pubkey -noout | openssl pkey -pubin -outform der \
  | openssl dgst -sha256
# CACert=: the certificate itself, to choose in the configurator
podman compose cp webdav:/etc/ssl/webdav/cert.pem webdav.pem
```

| Case | Lines | Expected |
|---|---|---|
| Nothing set | — | refused; the 3DS prints the `PinnedKey=` line for the key the server presented |
| Pinned key | `PinnedKey=` from the 3DS's message or the command above | works |
| Pinned key in another form | the same key as `AA:BB:…`, plain hex, or `sha256//…` (base64) | works |
| Wrong pin | `PinnedKey=` of another server (the FTP's key in `[WebDAV]`) | refused: "Its key is not the one PinnedKey= names", with the key to use instead |
| Certificate as pin | `openssl x509 -noout -fingerprint -sha256` of the certificate | works |
| Own CA, by address | `CACert=` from `webdav.pem` via the configurator, `Url=https://192.168.1.10:8443/dav/` | works: the certificate lists `HOST_IP` |
| Own CA, wrong name | the same, with `Url=` naming the machine by a host name the certificate lacks | refused |
| Own CA, other server | `CACert=` from the FTP's certificate in `[WebDAV]` | refused |
| Broken line | `PinnedKey=abc`, or `CACert=` cut short | startup check names it; the remote is left out |
| Both and `VerifyTLS=false` | all three lines | startup check: `CACert=` unused, `VerifyTLS=false` ignored; works through the pin |

### Anonymous

Leave the login out entirely. For FTP the `User=` line must be **missing**,
not empty: 3DSync only falls back to `anonymous` when the key is absent, and
an empty `User=` logs in as an empty name.

```ini
[SMB]
Server=192.168.1.10:1445
Share=3ds-public

[FTP]
Host=192.168.1.10
Port=2123
Path=pub                  ; the root is read-only, as on most anonymous servers
TLS=try

[WebDAV]
Url=http://192.168.1.10:8080/dav-anon/
```

On SMB, 3DSync passes no user, so libsmb2 falls back to `getlogin()` or,
failing that (presumably on the 3DS), `Guest`, with no password. A client
built from the pinned libsmb2 doing exactly that reaches `3ds-public`.

**Anonymous logins refused over TLS.** `ftp-anon` allows TLS for anonymous
logins so every `TLS=` mode can be tested. `ftp-anon-stock` (`Port=2124`)
keeps vsftpd's default (`allow_anon_ssl=NO`), as most real anonymous servers
do: it accepts `AUTH TLS`, then answers the login with `530 Anonymous
sessions may not use encryption`. libcurl reports that as a denied login.
Since #33, 3DSync then reconnects once without TLS for an anonymous user
and stays in plaintext for the run; before it, the run stopped with "Check
User and Password". A named user never falls back.

## Things worth trying

Beyond a clean sync:

- **Remote changed:** edit or replace a file through another client, then
  sync — 3DSync should download it, or prompt if the 3DS copy changed too.
- **Server gone mid-run:** `podman compose stop smb` (or `ftp`, `webdav`)
  during a sync.
- **Wrong password**, wrong share name, `Path=` pointing at a folder that
  does not exist yet.
- **Active FTP** (`Mode=active`): the server connects back to the 3DS, so a
  firewall or AP isolation between the two shows up here first.
