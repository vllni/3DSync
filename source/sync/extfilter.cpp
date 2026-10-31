#include "extfilter.h"

#include <ctype.h>
#include <set>

static std::string lower(const std::string &s)
{
    std::string out(s);
    for (char &c : out)
        c = (char)tolower((unsigned char)c);
    return out;
}

static bool isSeparator(char c)
{
    return c == ',' || isspace((unsigned char)c);
}

ExtensionFilter ExtensionFilter::parse(const std::string &spec)
{
    ExtensionFilter filter;
    size_t i = 0;
    while (i < spec.size())
    {
        while (i < spec.size() && isSeparator(spec[i]))
            i++;
        size_t start = i;
        while (i < spec.size() && !isSeparator(spec[i]))
            i++;
        std::string token = lower(spec.substr(start, i - start));

        // A bare "*" (or "*.*") asks for everything, whatever else is listed.
        if (token == "*" || token == "*.*")
            return ExtensionFilter();

        // "*.sav", ".sav" and "sav" are the same request.
        if (!token.empty() && token[0] == '*')
            token.erase(0, 1);
        if (!token.empty() && token[0] == '.')
            token.erase(0, 1);
        if (token.empty())
            continue;

        bool seen = false;
        for (auto &e : filter._extensions)
            seen = seen || e == token;
        if (!seen)
            filter._extensions.push_back(token);
    }
    return filter;
}

bool ExtensionFilter::matches(const std::string &path) const
{
    if (_extensions.empty())
        return true;

    size_t slash = path.rfind('/');
    std::string name = lower(slash == std::string::npos ? path : path.substr(slash + 1));
    for (auto &ext : _extensions)
    {
        // At least one character before the dot: ".sav" alone is a hidden
        // file with no extension, not a save.
        if (name.size() < ext.size() + 2)
            continue;
        size_t dot = name.size() - ext.size() - 1;
        if (name[dot] == '.' && name.compare(dot + 1, std::string::npos, ext) == 0)
            return true;
    }
    return false;
}

std::string ExtensionFilter::describe() const
{
    if (_extensions.empty())
        return "all files";
    std::string out;
    for (auto &ext : _extensions)
        out += (out.empty() ? "." : " .") + ext;
    return out;
}

// ---------------------------------------------------------------------------
// PathFilters
// ---------------------------------------------------------------------------
const ExtensionFilter &PathFilters::forEntry(const std::string &entryName) const
{
    auto it = perPath.find(lower(entryName));
    return it != perPath.end() ? it->second : defaults;
}

std::string PathFilters::describe() const
{
    return perPath.empty() ? defaults.describe() : "per path";
}

PathFilters parsePathFilters(const std::map<std::string, std::string> &values,
                             const std::vector<std::string> &entryNames)
{
    static const std::string SECTION = "filter=";
    std::set<std::string> known;
    for (auto &name : entryNames)
        known.insert(lower(name));

    PathFilters filters;
    for (auto &kv : values)
    {
        if (kv.first.compare(0, SECTION.size(), SECTION) != 0)
            continue;
        std::string key = lower(kv.first.substr(SECTION.size()));
        if (key == "extensions")
            filters.defaults = ExtensionFilter::parse(kv.second);
        else if (known.count(key))
            filters.perPath[key] = ExtensionFilter::parse(kv.second);
        else
            filters.unknown.push_back(key);
    }
    return filters;
}
