#include "fsutil.h"

#include "debug.h"
#include "pathutil.h"

#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <vector>


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
    std::string bakPath = localPath + ".3dsbak";
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

int compareFiles(const std::string &a, const std::string &b)
{
    FILE *fa = fopen(a.c_str(), "rb");
    if (!fa)
    {
        debugErrno("opening to compare", a);
        return -1;
    }
    FILE *fb = fopen(b.c_str(), "rb");
    if (!fb)
    {
        debugErrno("opening to compare", b);
        fclose(fa);
        return -1;
    }

    static const size_t CHUNK = 16 * 1024;
    std::vector<char> bufA(CHUNK), bufB(CHUNK);
    int result = 1;
    while (result == 1)
    {
        size_t na = fread(&bufA[0], 1, CHUNK, fa);
        size_t nb = fread(&bufB[0], 1, CHUNK, fb);
        if (na != nb || memcmp(&bufA[0], &bufB[0], na) != 0)
            result = 0;
        else if (na < CHUNK)
            break; // both ended here
    }
    if (ferror(fa) || ferror(fb))
    {
        debugf("reading %s or %s to compare failed\n", a.c_str(), b.c_str());
        result = -1;
    }
    fclose(fa);
    fclose(fb);
    return result;
}
