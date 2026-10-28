#include "configseal.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "debug.h"
#include "fsutil.h"

static bool readWhole(const std::string &path, std::string &out)
{
    FILE *f = fopen(path.c_str(), "rb");
    if (f == NULL)
        return false;
    out.clear();
    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        out.append(buf, n);
    bool ok = !ferror(f);
    fclose(f);
    return ok;
}

static bool flushToCard(FILE *f, const std::string &path)
{
    if (fflush(f) != 0)
    {
        debugErrno("fflush", path);
        return false;
    }
    // Not every devoptab implements fsync; a failure here is noted, not fatal.
    if (fsync(fileno(f)) != 0)
        debugErrno("fsync", path);
    return true;
}

static bool writeNew(const std::string &path, const std::string &text)
{
    FILE *f = fopen(path.c_str(), "wb");
    if (f == NULL)
    {
        debugErrno("fopen for writing", path);
        return false;
    }
    bool ok = fwrite(text.data(), 1, text.size(), f) == text.size() && flushToCard(f, path);
    fclose(f);
    if (!ok)
        debugErrno("writing", path);
    return ok;
}

// Rewrite the file's own clusters: open without truncating, write from the
// start, then cut the length to fit.
static bool overwriteInPlace(const std::string &path, const std::string &text)
{
    FILE *f = fopen(path.c_str(), "r+b");
    if (f == NULL)
    {
        debugErrno("fopen for overwriting", path);
        return false;
    }
    bool ok = fwrite(text.data(), 1, text.size(), f) == text.size() && flushToCard(f, path);
    if (ok && ftruncate(fileno(f), (off_t)text.size()) != 0)
    {
        debugErrno("ftruncate", path);
        ok = false;
    }
    fclose(f);
    return ok;
}

static void finishFromSealed(const std::string &path, const std::string &sealedPath)
{
    std::string text;
    if (!readWhole(sealedPath, text))
    {
        debugErrno("reading", sealedPath);
        return;
    }
    if (overwriteInPlace(path, text) || replaceLocalFile(sealedPath, path))
        remove(sealedPath.c_str());
}

void recoverConfigSeal(const std::string &path)
{
    std::string sealedPath = path + ".sealed", tmpPath = path + ".3dstmp";
    if (access(sealedPath.c_str(), F_OK) == 0)
    {
        printf("Finishing an interrupted update of 3DSync.ini\n");
        finishFromSealed(path, sealedPath);
    }
    if (access(tmpPath.c_str(), F_OK) == 0)
    {
        debugf("dropping an incomplete %s\n", tmpPath.c_str());
        remove(tmpPath.c_str());
    }
}

int sealConfigFile(const std::string &path, const SealKey &key, SealRandom random)
{
    std::string text;
    if (!readWhole(path, text))
        return 0; // no file: nothing to seal, and the caller reports it

    SealResult r = sealIniText(text, key, random);
    if (r.sealed == 0)
        return 0;

    std::string tmpPath = path + ".3dstmp", sealedPath = path + ".sealed";
    if (!writeNew(tmpPath, r.text))
    {
        remove(tmpPath.c_str());
        return -1;
    }
    if (rename(tmpPath.c_str(), sealedPath.c_str()) != 0)
    {
        debugErrno("rename to .sealed", tmpPath);
        remove(tmpPath.c_str());
        return -1;
    }
    if (overwriteInPlace(path, r.text))
    {
        remove(sealedPath.c_str());
        return r.sealed;
    }
    // The original could not be opened for writing: swap the sealed copy in
    // instead.  The plaintext's clusters are then only freed, not cleared,
    // but the file itself is sealed.
    if (replaceLocalFile(sealedPath, path))
        return r.sealed;
    return -1;
}
