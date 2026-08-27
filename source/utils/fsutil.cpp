#include "fsutil.h"

#include "pathutil.h"

#include <errno.h>
#include <string.h>
#include <sys/stat.h>


void mkdirs(const std::string &path)
{
    size_t pos = 1;
    while ((pos = path.find('/', pos)) != std::string::npos)
    {
        std::string dir = path.substr(0, pos);
        mkdir(dir.c_str(), 0755);
        pos++;
    }
    mkdir(path.c_str(), 0755);
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

    if (rename(tmpPath.c_str(), localPath.c_str()) != 0)
    {
        printf("Rename failed for %s: %s\n", localPath.c_str(), strerror(errno));
        if (hadExisting)
            rename(bakPath.c_str(), localPath.c_str()); // restore the original
        remove(tmpPath.c_str());
        return false;
    }

    if (hadExisting)
        remove(bakPath.c_str());
    return true;
}
