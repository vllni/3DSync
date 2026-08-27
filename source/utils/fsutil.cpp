#include "fsutil.h"

#include <errno.h>
#include <string.h>
#include <sys/stat.h>

#include <mbedtls/md5.h>
#include <mbedtls/sha256.h>

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

std::string computeMd5Hex(const std::string &path)
{
    FILE *fp = fopen(path.c_str(), "rb");
    if (!fp)
        return "";

    mbedtls_md5_context ctx;
    mbedtls_md5_init(&ctx);
    mbedtls_md5_starts(&ctx);

    unsigned char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
        mbedtls_md5_update(&ctx, buf, n);
    fclose(fp);

    unsigned char digest[16];
    mbedtls_md5_finish(&ctx, digest);
    mbedtls_md5_free(&ctx);

    char hex[33];
    for (int i = 0; i < 16; i++)
        snprintf(hex + i * 2, 3, "%02x", digest[i]);
    return std::string(hex, 32);
}

std::string computeDropboxHash(const std::string &path)
{
    FILE *fp = fopen(path.c_str(), "rb");
    if (!fp)
        return "";

    const size_t BLOCK_SIZE = 4 * 1024 * 1024;

    // The file is hashed one 4 MiB block at a time and only the digests are
    // kept, so nothing larger than the read buffer is ever held in memory.
    mbedtls_sha256_context overall;
    mbedtls_sha256_context block;
    mbedtls_sha256_init(&overall);
    mbedtls_sha256_init(&block);
    mbedtls_sha256_starts_ret(&overall, 0);
    mbedtls_sha256_starts_ret(&block, 0);

    unsigned char buf[16384];
    unsigned char digest[32];
    size_t inBlock = 0;
    size_t n;

    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
    {
        size_t offset = 0;
        while (offset < n)
        {
            size_t take = n - offset;
            if (take > BLOCK_SIZE - inBlock)
                take = BLOCK_SIZE - inBlock;

            mbedtls_sha256_update_ret(&block, buf + offset, take);
            offset += take;
            inBlock += take;

            if (inBlock == BLOCK_SIZE)
            {
                mbedtls_sha256_finish_ret(&block, digest);
                mbedtls_sha256_update_ret(&overall, digest, sizeof(digest));
                mbedtls_sha256_starts_ret(&block, 0);
                inBlock = 0;
            }
        }
    }
    fclose(fp);

    // Trailing partial block.  An empty file contributes no block digest at
    // all, which is what Dropbox reports for it.
    if (inBlock > 0)
    {
        mbedtls_sha256_finish_ret(&block, digest);
        mbedtls_sha256_update_ret(&overall, digest, sizeof(digest));
    }

    mbedtls_sha256_finish_ret(&overall, digest);
    mbedtls_sha256_free(&block);
    mbedtls_sha256_free(&overall);

    char hex[65];
    for (int i = 0; i < 32; i++)
        snprintf(hex + i * 2, 3, "%02x", digest[i]);
    return std::string(hex, 64);
}

FILE *openTempFor(const std::string &localPath, std::string &tmpPathOut)
{
    mkparents(localPath);
    tmpPathOut = localPath + ".3dstmp";
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
