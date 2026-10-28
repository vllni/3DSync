# Universal-Updater feed (UniStore v3) from the GitHub release list.
#
# Input: array of releases, each {tag_name, draft, prerelease, published_at,
#        body, assets: [{name, size, browser_download_url}]}.
# Arg:   $base — site URL, no trailing slash.
#
# storeInfo.revision is left at 0: Universal-Updater only re-downloads a store
# whose revision is strictly greater than the one it holds, so the number has
# to come from the feed already deployed, not from the releases (see pages.yml).
#
# Run locally:
#   jq -S --arg base https://3dsync.vllni.dev -f .github/unistore.jq releases.json

def ver: ltrimstr("v") | split(".") | map(tonumber? // 0);
# Same format Universal-DB uses. Universal-Updater compares it as a string
# against the one recorded at install time to flag an update, so it must sort.
def stamp: fromdateiso8601 | strftime("%Y-%m-%d at %H:%M:%S (UTC)");
def mib: (. / 1048576 * 10 | round / 10 | tostring) + " MiB";
def asset($r; $ext): $r.assets[] | select(.name == "3DSync-\($r.tag_name).\($ext)");
def pre($r): if $r.prerelease then {type: "prerelease"} else {} end;
def cia($r): asset($r; "cia") as $a | {
  script: [
    {type: "downloadFile", file: $a.browser_download_url, output: "/3DSync.cia"},
    {type: "installCia", file: "/3DSync.cia"},
    {type: "deleteFile", file: "/3DSync.cia"}
  ], size: ($a.size | mib) } + pre($r);
# "%3DSX%/<name>.3dsx" and nothing deeper: Universal-Updater rewrites it with a
# greedy regex into /3ds/<name>.3dsx or, with its "3DSX in folder" option,
# /3ds/<name>/<name>.3dsx. A subfolder here would be doubled by that option.
def tdsx($r): asset($r; "3dsx") as $a | {
  script: [
    {type: "downloadFile", file: $a.browser_download_url, output: "%3DSX%/3DSync.3dsx"}
  ], size: ($a.size | mib) } + pre($r);

# Releases before v0.4.9 published unversioned asset names (3DSync.cia) and
# are left out rather than guessed at.
[ .[] | select(.draft | not)
      | .tag_name as $t
      | select(any(.assets[]; .name == "3DSync-\($t).cia"))
      | select(any(.assets[]; .name == "3DSync-\($t).3dsx")) ]
| sort_by(.tag_name | ver) | reverse
| . as $rels
| if length == 0 then error("no releases with 3DSync assets") else . end
| ([ $rels[] | select(.prerelease | not) ][0] // $rels[0]) as $latest
| {
  storeInfo: {
    title: "3DSync", author: "vllni",
    description: "Sync 3DS saves with Google Drive, Dropbox, WebDAV, FTP and SMB",
    file: "3dsync.unistore",
    url: "\($base)/unistore/3dsync.unistore",
    infoURL: "\($base)/unistore/3dsync-info.json",
    sheet: "3dsync.t3x", sheetURL: "\($base)/unistore/3dsync.t3x",
    version: 3, revision: 0
  },
  storeContent: [
    ( { info: {
          title: "3DSync", author: "vllni",
          description: "Sync saves and files between your 3DS, the cloud and your PC",
          version: $latest.tag_name,
          console: ["3DS"], category: ["utility"], license: "mit",
          icon_index: 0, sheet_index: 0,
          last_updated: ($latest.published_at | stamp),
          # GitHub stores notes with CRLF; the 3DS font draws the CR.
          releasenotes: (($latest.body // "")
            | gsub("\r"; "")
            | sub("\\s*<!-- 3dsync-qr:start -->[\\s\\S]*$"; "")),
          # UNIQUE_ID 0xF5555 from the Makefile: marks the CIA as installed.
          title_ids: [1004885],
          installed_files: ["%3DSX%/3DSync.3dsx"]
        } }
      + ( [ $rels[] as $r
            | (if $r.tag_name == $latest.tag_name then "" else "[\($r.tag_name)] " end) as $p
            | {("\($p)3DSync.cia"): cia($r)}, {("\($p)3DSync.3dsx"): tdsx($r)} ]
          | add ) )
  ]
}
