#include "remoteconfig.h"

#include <ctype.h>

namespace
{

struct TypeInfo
{
    RemoteType type;
    const char *section;     // INIReader's lowercased spelling
    const char *displayName; // SyncProvider::name() of the backend
};

// In connect order.
const TypeInfo TYPES[] = {{REMOTE_DROPBOX, "dropbox", "Dropbox"},
                          {REMOTE_DRIVE, "googledrive", "Drive"},
                          {REMOTE_SMB, "smb", "SMB"},
                          {REMOTE_FTP, "ftp", "FTP"},
                          {REMOTE_WEBDAV, "webdav", "WebDAV"}};

const TypeInfo *typeNamed(const std::string &section)
{
    for (const TypeInfo &t : TYPES)
        if (section == t.section)
            return &t;
    return 0;
}

std::string valueOf(const std::map<std::string, std::string> &values,
                    const std::string &section, const char *key)
{
    auto it = values.find(section + "=" + key);
    return it == values.end() ? std::string() : it->second;
}

ConfigIssue issue(const std::string &section, const std::string &brief,
                  const std::vector<std::string> &lines, bool leftOut)
{
    ConfigIssue i;
    i.section = section;
    i.brief = "[" + section + "] " + brief;
    i.lines = lines;
    i.leftOut = leftOut;
    return i;
}

// What a section lacks before a sync can try it, as an issue; false when it
// has everything.  The required keys are the ones the app has always checked.
bool missingKeys(RemoteType type, const std::map<std::string, std::string> &values,
                 const std::string &section, ConfigIssue &out)
{
    std::string brief, what;
    switch (type)
    {
    case REMOTE_DROPBOX:
    case REMOTE_DRIVE:
        if (!valueOf(values, section, "token").empty() ||
            !valueOf(values, section, "refreshtoken").empty())
            return false;
        out = issue(section, "needs a login",
                    {"A remote has no login:", "  [" + section + "]",
                     "  Log in again with the configurator", "  and copy its RefreshToken= here."},
                    true);
        return true;
    case REMOTE_SMB:
    {
        bool server = !valueOf(values, section, "server").empty();
        bool share = !valueOf(values, section, "share").empty();
        if (server && share)
            return false;
        brief = !server && !share ? "needs Server=, Share=" : (!server ? "needs Server=" : "needs Share=");
        what = !server && !share ? "  Add Server= and Share=."
                                 : (!server ? "  Add Server= (the server address)."
                                            : "  Add Share= (the share name).");
        break;
    }
    case REMOTE_FTP:
        if (!valueOf(values, section, "host").empty())
            return false;
        brief = "needs Host=";
        what = "  Add Host= (the server address).";
        break;
    case REMOTE_WEBDAV:
        if (!valueOf(values, section, "url").empty())
            return false;
        brief = "needs Url=";
        what = "  Add Url= (the WebDAV address).";
        break;
    }
    out = issue(section, brief, {"A remote is missing a setting:", "  [" + section + "]", what},
                true);
    return true;
}

// Split on commas and whitespace, like [Filter] Extensions=, lowercased.
std::set<std::string> splitNames(const std::string &list)
{
    std::set<std::string> out;
    std::string current;
    for (char c : list)
    {
        if (c == ',' || isspace((unsigned char)c))
        {
            if (!current.empty())
                out.insert(current);
            current.clear();
        }
        else
            current += (char)tolower((unsigned char)c);
    }
    if (!current.empty())
        out.insert(current);
    return out;
}

std::string lower(const std::string &s)
{
    std::string out = s;
    for (char &c : out)
        c = (char)tolower((unsigned char)c);
    return out;
}

} // namespace

std::string RemoteConfig::displayName() const
{
    std::string name = "?";
    for (const TypeInfo &t : TYPES)
        if (t.type == type)
            name = t.displayName;
    return label.empty() ? name : name + " (" + label + ")";
}

bool RemoteConfig::syncs(const std::string &entryName) const
{
    return only.empty() || only.count(lower(entryName)) > 0;
}

RemoteConfigs parseRemotes(const std::map<std::string, std::string> &values,
                           const std::vector<std::string> &entryNames)
{
    RemoteConfigs result;

    // Every section that names a remote, once each.  GetValues() is sorted,
    // so "smb" comes before "smb:backup".
    std::set<std::string> sections;
    for (auto &kv : values)
    {
        size_t eq = kv.first.find('=');
        if (eq != std::string::npos)
            sections.insert(kv.first.substr(0, eq));
    }

    std::set<std::string> known;
    for (auto &name : entryNames)
        known.insert(lower(name));

    std::vector<RemoteConfig> found;
    for (auto &section : sections)
    {
        size_t colon = section.find(':');
        std::string typePart = section.substr(0, colon);
        const TypeInfo *type = typeNamed(typePart);
        if (!type)
        {
            // [Paths], [Filter], [Security] and the like are not remotes, but
            // a labelled section can only have been meant as one.
            if (colon != std::string::npos)
                result.issues.push_back(issue(
                    section, "is no remote type",
                    {"Not a remote type, section left out:", "  [" + section + "]",
                     "  Use Dropbox, GoogleDrive, SMB, FTP", "  or WebDAV before the ':'."},
                    true));
            continue;
        }

        RemoteConfig remote;
        remote.type = type->type;
        remote.section = section;
        remote.label = colon == std::string::npos ? "" : section.substr(colon + 1);
        if (colon != std::string::npos && remote.label.empty())
        {
            result.issues.push_back(issue(section, "has an empty label",
                                          {"A remote label is empty, section left out:",
                                           "  [" + section + "]",
                                           "  Write a name after the ':' or drop it."},
                                          true));
            continue;
        }
        ConfigIssue missing;
        if (missingKeys(remote.type, values, section, missing))
        {
            result.issues.push_back(missing);
            continue;
        }

        std::string role = lower(valueOf(values, section, "role"));
        if (role == "backup")
            remote.role = ROLE_BACKUP;
        else if (role == "source")
            remote.role = ROLE_SOURCE;
        else if (role.empty() || role == "sync")
            remote.role = ROLE_SYNC;
        else
        {
            // Not a guess at what was meant: a misspelt "backup" read as "sync"
            // would start downloading from a remote meant to only receive.
            result.issues.push_back(issue(section, "Role= is not known",
                                          {"Role= is not sync, backup or source,",
                                           "  so this remote is left out:",
                                           "  [" + section + "]"},
                                          true));
            continue;
        }

        std::string verify = lower(valueOf(values, section, "verifytls"));
        bool canSkipVerify = remote.type == REMOTE_WEBDAV || remote.type == REMOTE_FTP;
        if (verify == "false" || verify == "no" || verify == "0" || verify == "off")
        {
            if (canSkipVerify)
                remote.verifyTls = false;
            else
                result.issues.push_back(issue(section, "VerifyTLS=false ignored",
                                              {"VerifyTLS=false is ignored in",
                                               "  [" + section + "]",
                                               "  Its certificate is always checked."},
                                              false));
        }
        else if (!verify.empty() && verify != "true" && verify != "yes" && verify != "1" &&
                 verify != "on")
            result.issues.push_back(issue(section, "VerifyTLS= unknown",
                                          {"VerifyTLS= is neither true nor false in",
                                           "  [" + section + "]",
                                           "  Certificates are checked until fixed."},
                                          false));

        if (remote.type == REMOTE_FTP)
        {
            std::string tls = lower(valueOf(values, section, "tls"));
            if (tls.empty() || tls == "try")
                remote.ftpTls = "try";
            else if (tls == "none" || tls == "implicit")
                remote.ftpTls = tls;
            else if (tls == "require" || tls == "required" || tls == "explicit")
                remote.ftpTls = "require";
            else
            {
                remote.ftpTls = "require";
                result.issues.push_back(issue(section, "TLS= is not known",
                                              {"TLS= is not none, try, require or",
                                               "  implicit in [" + section + "]",
                                               "  TLS is required until that is fixed."},
                                              false));
            }
        }

        remote.only = splitNames(valueOf(values, section, "only"));
        for (auto &name : remote.only)
            if (!known.count(name))
                result.issues.push_back(issue(section, "Only= names no path",
                                              {"Only= names a path that is not configured:",
                                               "  " + name, "  in [" + section + "]",
                                               "  Use a name from [Paths]."},
                                              false));

        found.push_back(remote);
    }

    // Type order first; within a type the sorted section names already put
    // the unnamed one before the labelled ones.
    for (const TypeInfo &t : TYPES)
        for (auto &remote : found)
            if (remote.type == t.type)
                result.remotes.push_back(remote);
    return result;
}

std::string remoteNames(const std::vector<RemoteConfig> &remotes)
{
    std::string out;
    for (auto &remote : remotes)
        out += (out.empty() ? "" : ", ") + remote.displayName();
    return out;
}

std::string manifestScope(const RemoteConfig &remote, const std::string &providerPrefix)
{
    return remote.label.empty() ? providerPrefix : providerPrefix + "#" + remote.label;
}
