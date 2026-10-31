#ifndef SYNC_EXTFILTER_H
#define SYNC_EXTFILTER_H

#include <map>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// ExtensionFilter  — which files a sync considers, by file name ending
// ---------------------------------------------------------------------------
// One per sync path, from [Filter] in 3DSync.ini (see PathFilters).  Empty
// (the default) lets
// every file through.  Otherwise a file is synced only when its name ends in
// one of the listed extensions, compared without regard to case, so
// "sav, BIN" matches 001.sav and data.bin.  A listed "tar.gz" matches by the
// whole ending, not just the part after the last dot.
//
// The list is split on commas and whitespace — not on ';', which inih reads
// as the start of a comment once it follows a space.  "sav", ".sav" and
// "*.sav" all mean the same; "*" or "*.*" means every file.
//
// Applied to both sides: a local file that does not match is not uploaded,
// and a remote one is not downloaded.  The manifest entries of files the
// filter now hides are left as they are, so widening it again later resumes
// from the same baseline instead of treating those files as new.
// ---------------------------------------------------------------------------
class ExtensionFilter
{
public:
    ExtensionFilter() {}

    static ExtensionFilter parse(const std::string &spec);

    bool allowsAll() const { return _extensions.empty(); }

    // relPath or a full path; only the part after the last '/' is looked at.
    bool matches(const std::string &path) const;

    // Lowercased, without the leading dot, in the order given, no repeats.
    const std::vector<std::string> &extensions() const { return _extensions; }

    // "all files", or ".sav .bin" — for the menu and the log.
    std::string describe() const;

private:
    std::vector<std::string> _extensions;
};

// ---------------------------------------------------------------------------
// PathFilters  — the [Filter] section as a whole
// ---------------------------------------------------------------------------
//   [Filter]
//   Extensions=sav, bin    every path that has no line of its own
//   roms/nds/saves=dsv     this path only, by its name in [Paths] & co.
//   screenshots=*          every file in this one, whatever the default
//
// A path's own line replaces the default rather than adding to it.  Names
// are compared lowercased, as INIReader keys them, which also means a path
// named "Extensions" cannot have one: that key is the default.
// ---------------------------------------------------------------------------
struct PathFilters
{
    ExtensionFilter defaults;
    std::map<std::string, ExtensionFilter> perPath; // by lowercased path name
    // [Filter] keys that name no configured path, lowercased.
    std::vector<std::string> unknown;

    const ExtensionFilter &forEntry(const std::string &entryName) const;

    // For the menu card: the default's describe(), or "per path" once any
    // path has its own.
    std::string describe() const;
};

// values: INIReader::GetValues(), keyed "section=key".  entryNames: the
// configured sync paths' names, to tell a typo from a path.
PathFilters parsePathFilters(const std::map<std::string, std::string> &values,
                             const std::vector<std::string> &entryNames);

#endif
