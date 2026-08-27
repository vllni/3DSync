#include "mocks.h"

#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <utime.h>

TempDir::TempDir()
{
    char templatePath[] = "/tmp/3dsync-tests-XXXXXX";
    const char *created = mkdtemp(templatePath);
    _path = (created != NULL) ? created : "/tmp/3dsync-tests-fallback";
    if (created == NULL)
        mkdir(_path.c_str(), 0755);
}

TempDir::~TempDir()
{
    // Only ever holds files this test wrote, under a mkdtemp path.
    std::string command = "rm -rf '" + _path + "'";
    if (system(command.c_str()) != 0)
        return;
}

void TempDir::write(const std::string &relPath, const std::string &content,
                    long long mtime)
{
    std::string full = _path + relPath;

    size_t pos = _path.size();
    while ((pos = full.find('/', pos + 1)) != std::string::npos)
        mkdir(full.substr(0, pos).c_str(), 0755);

    FILE *fp = fopen(full.c_str(), "wb");
    if (!fp)
        return;
    fwrite(content.data(), 1, content.size(), fp);
    fclose(fp);

    if (mtime != 0)
    {
        struct utimbuf times;
        times.actime = (time_t)mtime;
        times.modtime = (time_t)mtime;
        utime(full.c_str(), &times);
    }
}

std::string TempDir::read(const std::string &relPath) const
{
    return MockRemote::readLocal(_path + relPath);
}

bool TempDir::exists(const std::string &relPath) const
{
    struct stat st;
    return stat((_path + relPath).c_str(), &st) == 0;
}

long long TempDir::mtimeOf(const std::string &relPath) const
{
    struct stat st;
    if (stat((_path + relPath).c_str(), &st) != 0)
        return -1;
    return (long long)st.st_mtime;
}
