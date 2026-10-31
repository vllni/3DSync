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

struct RemoteConfigs
{
    // Configured remotes in connect order: by type (Dropbox, Drive, SMB,
    // FTP, WebDAV), the unnamed one of a type before its labelled ones.
    std::vector<RemoteConfig> remotes;
    // Mistakes worth telling the user about, each already split into console
    // lines under 50 columns, the first unindented.
    std::vector<std::vector<std::string> > warnings;
    // Sections meant as remotes but left out, an unknown type or an empty
    // label ("smbx:nas").  Each also has a warning; the run lists them as
    // not synced, so the summary does not pass over them.
    std::vector<std::string> ignored;
    // Remote sections left out for lack of their required keys ("smb:enc").
    // Not a warning: the configurator writes a section only once it is filled
    // in, so an empty one is someone's work in progress.  For debug output.
    std::vector<std::string> incomplete;
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
