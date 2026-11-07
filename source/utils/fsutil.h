#ifndef UTILS_FSUTIL_H
#define UTILS_FSUTIL_H

#include <cstdio>
#include <string>
#include <vector>

// Create every directory component of path.
void mkdirs(const std::string &path);

// Create the parent directories of a file path.
void mkparents(const std::string &filePath);

// Open "<localPath>.3dstmp" for writing, creating parent directories.
// Returns NULL on failure; tmpPathOut receives the temp path.
FILE *openTempFor(const std::string &localPath, std::string &tmpPathOut);

// Move tmpPath onto localPath.  FAT (the 3DS SD card) cannot rename over an
// existing file, so the destination is parked as "<localPath>.3dsbak" first and
// restored if the swap fails.  Removes tmpPath on failure.
bool replaceLocalFile(const std::string &tmpPath, const std::string &localPath);

// Suffix of the file replaceLocalFile() parks the previous version under.
extern const char *BACKUP_SUFFIX;

// The files under base, as '/'-prefixed paths relative to it, descending into
// subfolders when recursive.  Leftovers of an interrupted transfer are not
// files to sync: "<name>.3dstmp" is skipped, and so is "<name>.3dsbak" — which
// is renamed back to <name> first when <name> is missing, since that means the
// swap in replaceLocalFile() stopped halfway and the backup is the last good
// copy.  Says so when base is not there.
std::vector<std::string> listLocalFiles(const std::string &base, bool recursive);

// What a finished download is checked against before it may replace the
// local file.  received < 0 skips the size check, an empty expectedHash (or no
// hashOf) the content check.
struct DownloadCheck
{
    DownloadCheck() : received(-1), hashOf(NULL) {}
    long long received;    // bytes the transfer handed to fwrite()
    std::string (*hashOf)(const std::string &path);
    std::string expectedHash; // what hashOf() must return for the file
};

// Closes fp — a download's temp file from openTempFor() — and moves it onto
// localPath only when all of it reached the card: the flush and close
// succeeded, its size on the card is what was received, and its hash is the
// one the remote published.  Otherwise removes it, says why, and returns
// false, leaving the local file as it was (or absent).  A full SD card fails the last
// flush, which nothing checked before, and a truncated save replaced a good one.
bool finishDownload(FILE *fp, const std::string &tmpPath, const std::string &localPath,
                    const DownloadCheck &check);

#endif
