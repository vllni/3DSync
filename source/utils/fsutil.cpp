#include "fsutil.h"

#include "console.h"
#include "debug.h"
#include "pathutil.h"

#include <dirent.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>

const char *BACKUP_SUFFIX = ".3dsbak";

static bool endsWith(const std::string &s, const char *suffix)
{
    size_t n = strlen(suffix);
    return s.size() > n && s.compare(s.size() - n, n, suffix) == 0;
}


// EEXIST is the normal outcome for every component but the last, so only other
// errors are worth a word.  They are worth one: a write-protected or full SD
// card shows up here first, and the caller only ever sees "cannot create temp
// file" one layer up.
static void mkdirStep(const std::string &dir)
{
    if (mkdir(dir.c_str(), 0755) == 0 || errno == EEXIST)
        return;
    debugErrno("mkdir", dir);
}

void mkdirs(const std::string &path)
{
    size_t pos = 1;
    while ((pos = path.find('/', pos)) != std::string::npos)
    {
        std::string dir = path.substr(0, pos);
        mkdirStep(dir);
        pos++;
    }
    mkdirStep(path);
}

void mkparents(const std::string &filePath)
{
    size_t slash = filePath.rfind('/');
    if (slash != std::string::npos && slash > 0)
        mkdirs(filePath.substr(0, slash));
}

FILE *openTempFor(const std::string &localPath, std::string &tmpPathOut)
{
    mkparents(localPath);
    tmpPathOut = localPath + TEMP_SUFFIX;
    FILE *fp = fopen(tmpPathOut.c_str(), "wb");
    if (!fp)
        printf("Cannot create temp file %s: %s\n", tmpPathOut.c_str(), strerror(errno));
    return fp;
}

bool replaceLocalFile(const std::string &tmpPath, const std::string &localPath)
{
    std::string bakPath = localPath + BACKUP_SUFFIX;
    bool hadExisting = (rename(localPath.c_str(), bakPath.c_str()) == 0);
    if (!hadExisting && errno != ENOENT)
        debugErrno("parking the existing file as .3dsbak", localPath);

    if (rename(tmpPath.c_str(), localPath.c_str()) != 0)
    {
        printf("Rename failed for %s: %s\n", localPath.c_str(), strerror(errno));
        // Losing the backup would lose the save, so this one is loud even in a
        // quiet run.
        if (hadExisting && rename(bakPath.c_str(), localPath.c_str()) != 0)
            printf("The previous version is left as %s\n", bakPath.c_str());
        remove(tmpPath.c_str());
        return false;
    }

    // A backup left behind is harmless but confusing, and it is also a sign the
    // card is refusing writes.
    if (hadExisting && remove(bakPath.c_str()) != 0)
        debugErrno("removing the backup", bakPath);
    return true;
}

static void listInto(const std::string &base, const std::string &rel, bool recursive,
                     std::vector<std::string> &out)
{
    std::string dirPath = base + rel;
    DIR *dir = opendir(dirPath.c_str());
    if (!dir)
    {
        debugErrno("opendir", dirPath);
        return;
    }

    std::vector<std::string> names;
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL)
    {
        std::string name = ent->d_name;
        if (name != "." && name != "..")
            names.push_back(name);
    }
    closedir(dir);

    for (auto &name : names)
    {
        std::string childRel = rel + "/" + name;
        std::string childPath = base + childRel;
        struct stat st = {};
        if (stat(childPath.c_str(), &st) != 0)
        {
            debugErrno("stat", childPath);
            continue;
        }
        if (S_ISDIR(st.st_mode))
        {
            if (recursive)
                listInto(base, childRel, true, out);
            continue;
        }
        if (!S_ISREG(st.st_mode))
            continue;

        if (isTempTransferName(name))
        {
            debugf("skipping %s, left by an interrupted download\n", childPath.c_str());
            continue;
        }
        if (endsWith(name, BACKUP_SUFFIX))
        {
            std::string origRel = childRel.substr(0, childRel.size() - strlen(BACKUP_SUFFIX));
            std::string origPath = base + origRel;
            struct stat orig = {};
            if (stat(origPath.c_str(), &orig) == 0)
            {
                debugf("skipping %s, a leftover backup\n", childPath.c_str());
                continue;
            }
            if (rename(childPath.c_str(), origPath.c_str()) != 0)
            {
                debugErrno("restoring the backup", childPath);
                continue;
            }
            printf("Restored a save an interrupted download\n");
            printf("had moved aside:\n  %s\n", origPath.c_str());
            childRel = origRel;
        }
        out.push_back(childRel);
    }
}

std::vector<std::string> listLocalFiles(const std::string &base, bool recursive)
{
    std::vector<std::string> files;
    struct stat st = {};
    bool found = (stat(base.c_str(), &st) == 0);
    if (!found || !S_ISDIR(st.st_mode))
    {
        printf("Folder %s not found\n", base.c_str());
        if (!found)
            debugErrno("stat", base);
        else
            debugf("%s is a file, not a folder\n", base.c_str());
        return files;
    }
    listInto(base, "", recursive, files);
    return files;
}

bool finishDownload(FILE *fp, const std::string &tmpPath, const std::string &localPath,
                    const DownloadCheck &check)
{
    bool written = (fflush(fp) == 0 && ferror(fp) == 0);
    int writeErrno = errno;
    if (fclose(fp) != 0 && written)
    {
        written = false;
        writeErrno = errno;
    }

    const char *problem = NULL;
    if (!written)
    {
        problem = "SD card full?";
        debugf("finishing %s: %s\n", tmpPath.c_str(), strerror(writeErrno));
    }

    struct stat st = {};
    if (!problem && check.received >= 0)
    {
        if (stat(tmpPath.c_str(), &st) != 0)
        {
            problem = "not readable";
            debugErrno("stat", tmpPath);
        }
        else if ((long long)st.st_size != check.received)
        {
            problem = "incomplete";
            debugf("%s: %lld bytes received, %lld on the card\n", tmpPath.c_str(),
                   check.received, (long long)st.st_size);
        }
    }

    if (!problem && check.hashOf && !check.expectedHash.empty())
    {
        std::string actual = check.hashOf(tmpPath);
        if (actual != check.expectedHash)
        {
            problem = "wrong checksum";
            debugf("%s: hash %s, the remote says %s\n", tmpPath.c_str(),
                   actual.empty() ? "(unreadable)" : actual.c_str(),
                   check.expectedHash.c_str());
        }
    }

    if (problem)
    {
        printf(CONSOLE_RED "Download not saved (%s):\n" CONSOLE_RESET, problem);
        printf("  %s\n", localPath.c_str());
        printf("  The file on the 3DS is unchanged.\n");
        if (remove(tmpPath.c_str()) != 0)
            debugErrno("removing", tmpPath);
        return false;
    }
    return replaceLocalFile(tmpPath, localPath);
}
