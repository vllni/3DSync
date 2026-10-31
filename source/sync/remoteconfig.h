#ifndef SYNC_REMOTECONFIG_H
#define SYNC_REMOTECONFIG_H

#include <map>
#include <set>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// RemoteConfig  — which remotes 3DSync.ini configures, and how each takes part
// ---------------------------------------------------------------------------
// A remote is a section named after its type, "[SMB]", optionally with a
// label for a second one of the same type: "[SMB:Backup]".  The unnamed one
// keeps the manifest scope it always had; a labelled one gets its own (see
// manifestScope()), so two shares on one server keep separate state.
//
// Two keys apply to any remote section:
//   Role=sync|backup  backup only ever receives: it is never the source of a
//                     change, is never asked about, and files only it holds
//                     are left alone.  Default sync.
//   Only=saves, gba   the [Paths]-style names this remote takes part in.
//                     Missing or empty: every one.  Commas and whitespace
//                     separate, as in [Filter] Extensions=.
//
// INIReader lowercases section and key names, so labels and the names in
// Only= are compared in lower case and shown that way.
//
// Free of INIReader and libctru: it reads the map GetValues() returns, so it
// is tested on the host.
// ---------------------------------------------------------------------------
enum RemoteType
{
    REMOTE_DROPBOX,
    REMOTE_DRIVE,
    REMOTE_SMB,
    REMOTE_FTP,
    REMOTE_WEBDAV
};

enum RemoteRole
{
    ROLE_SYNC,
    ROLE_BACKUP
};

struct RemoteConfig
{
    RemoteType type;
    std::string section; // as INIReader keys it: "smb" or "smb:backup"
    std::string label;   // "" for the unnamed one
    RemoteRole role;
    std::set<std::string> only; // entry names; empty = every entry

    // "SMB", or "SMB (backup)" for a labelled one.  The name the log, the
    // conflict prompt and the summary use.
    std::string displayName() const;

    // Whether this remote takes part in the entry of that name.
    bool syncs(const std::string &entryName) const;
};

// Something in a remote section the user has to fix.  Shown before a sync
// starts, so a remote that is set up wrongly never just goes missing.
struct ConfigIssue
{
    std::string section; // as INIReader keys it: "smb:enc"
    // One line for the bottom screen, at most about 40 characters:
    // "[smb:enc] needs Share=".
    std::string brief;
    // For the log: what is wrong and what to do, each line under 50
    // columns, the first unindented.
    std::vector<std::string> lines;
    // The remote is not synced at all, rather than synced with a default.
    bool leftOut;
};

struct RemoteConfigs
{
    // Configured remotes in connect order: by type (Dropbox, Drive, SMB,
    // FTP, WebDAV), the unnamed one of a type before its labelled ones.
    std::vector<RemoteConfig> remotes;
    // In section order.  A remote section missing a required key, one of an
    // unknown type behind a ':', an empty label, a bad Role= and an Only=
    // naming no configured path.
    std::vector<ConfigIssue> issues;
};

// values: INIReader::GetValues(), keyed "section=key".  entryNames: the names
// of the configured sync paths, to check Only= against.
RemoteConfigs parseRemotes(const std::map<std::string, std::string> &values,
                           const std::vector<std::string> &entryNames);

// "Dropbox, SMB (backup)" — for the menu card.
std::string remoteNames(const std::vector<RemoteConfig> &remotes);

// The manifest key prefix for a remote: the provider's own for the unnamed
// one, so nothing it synced before is forgotten, and that plus "#<label>" for
// a labelled one.  Renaming a label therefore starts its state afresh.
std::string manifestScope(const RemoteConfig &remote, const std::string &providerPrefix);

#endif
