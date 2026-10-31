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

// What a section needs before a sync can try it.  The same rules the app has
// always used for the unnamed sections.
bool isConfigured(RemoteType type, const std::map<std::string, std::string> &values,
                  const std::string &section)
{
    switch (type)
    {
    case REMOTE_DROPBOX:
    case REMOTE_DRIVE:
        return !valueOf(values, section, "token").empty() ||
               !valueOf(values, section, "refreshtoken").empty();
    case REMOTE_SMB:
        return !valueOf(values, section, "server").empty() &&
               !valueOf(values, section, "share").empty();
    case REMOTE_FTP:
        return !valueOf(values, section, "host").empty();
    case REMOTE_WEBDAV:
        return !valueOf(values, section, "url").empty();
    }
    return false;
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
            {
                result.warnings.push_back(
                    {"Unknown remote type, section ignored:", "  [" + section + "]"});
                result.ignored.push_back(section);
            }
            continue;
        }

        RemoteConfig remote;
        remote.type = type->type;
        remote.section = section;
        remote.label = colon == std::string::npos ? "" : section.substr(colon + 1);
        if (colon != std::string::npos && remote.label.empty())
        {
            result.warnings.push_back(
                {"A remote label is empty, section ignored:", "  [" + section + "]"});
            result.ignored.push_back(section);
            continue;
        }
        if (!isConfigured(remote.type, values, section))
        {
            result.incomplete.push_back(section);
            continue;
        }

        std::string role = lower(valueOf(values, section, "role"));
        remote.role = role == "backup" ? ROLE_BACKUP : ROLE_SYNC;
        if (!role.empty() && role != "sync" && role != "backup")
            result.warnings.push_back({"Role= is neither sync nor backup in",
                                       "  [" + section + "]", "  so it syncs both ways."});

        remote.only = splitNames(valueOf(values, section, "only"));
        for (auto &name : remote.only)
            if (!known.count(name))
                result.warnings.push_back({"Only= names a path that is not configured:",
                                           "  " + name, "  in [" + section + "]"});

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
