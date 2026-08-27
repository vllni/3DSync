#ifndef UTILS_FSUTIL_H
#define UTILS_FSUTIL_H

#include <cstdio>
#include <string>

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

#endif
