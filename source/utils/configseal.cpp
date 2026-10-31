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

bool sealedCopyBelongsTo(const std::string &current, const std::string &sealed,
                         const SealKey *key)
{
    // Overwritten completely; only the removal of the copy was missed.
    if (current == sealed)
        return true;
    // Not overwritten at all: the same file once every sealed value in both is
    // opened (a secret that was plain in one is sealed in the other).
    std::string openCurrent, openSealed;
    if (key && openIniText(current, *key, openCurrent) && openIniText(sealed, *key, openSealed) &&
        openCurrent == openSealed)
        return true;
    // Partly overwritten: the file starts with the copy's bytes, and those go
    // past the first value sealed in it — a random nonce that only this seal
    // wrote.  A file put there since cannot contain it.
    size_t same = 0;
    while (same < current.size() && same < sealed.size() && current[same] == sealed[same])
        same++;
    size_t first = sealed.find("enc:v1:");
    // The prefix, plus enough of the nonce's base64 (16 characters = 12 bytes)
    // that nothing else could match it by chance.
    return first != std::string::npos && same >= first + 7 + 16;
}

bool recoverConfigSeal(const std::string &path, const SealKey *key)
{
    std::string sealedPath = path + ".sealed", tmpPath = path + ".3dstmp";
    bool kept = true;
    if (access(sealedPath.c_str(), F_OK) == 0)
    {
        std::string current, sealed;
        bool haveCurrent = readWhole(path, current);
        if (!readWhole(sealedPath, sealed))
            debugErrno("reading", sealedPath);
        else if (!haveCurrent || sealedCopyBelongsTo(current, sealed, key))
        {
            printf("Finishing an interrupted update of 3DSync.ini\n");
            finishFromSealed(path, sealedPath);
        }
        else
        {
            // The user has put another file there since: theirs wins.
            printf("3DSync.ini was replaced since its last\n"
                   "update; a leftover copy was discarded.\n");
            remove(sealedPath.c_str());
            kept = false;
        }
    }
    if (access(tmpPath.c_str(), F_OK) == 0)
    {
        debugf("dropping an incomplete %s\n", tmpPath.c_str());
        remove(tmpPath.c_str());
    }
    return kept;
}

int sealConfigFile(const std::string &path, const SealKey &key, SealRandom random,
                   std::vector<std::string> *skipped)
{
    std::string text;
    if (!readWhole(path, text))
        return 0; // no file: nothing to seal, and the caller reports it

    SealResult r = sealIniText(text, key, random);
    if (skipped)
        *skipped = r.skippedFields;
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
