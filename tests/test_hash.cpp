// Content hashing.  Both hashes decide whether a file is re-uploaded, and the
// Dropbox one has a block structure that is easy to get subtly wrong — an
// off-by-one at a 4 MiB boundary would never match the server, so every sync
// would re-upload every large file forever.

#include "framework.h"

#include "../source/utils/hash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string>

namespace
{

class ScratchFile
{
public:
    ScratchFile()
    {
        char templatePath[] = "/tmp/3dsync-hash-XXXXXX";
        const char *dir = mkdtemp(templatePath);
        _dir = (dir != NULL) ? dir : "/tmp";
        _path = _dir + "/data.bin";
    }

    ~ScratchFile()
    {
        std::string command = "rm -rf '" + _dir + "'";
        if (system(command.c_str()) != 0)
            return;
    }

    const std::string &path() const { return _path; }

    void write(const std::string &contents)
    {
        FILE *fp = fopen(_path.c_str(), "wb");
        if (!fp)
            return;
        fwrite(contents.data(), 1, contents.size(), fp);
        fclose(fp);
    }

    // Write count copies of a byte without building the whole string.
    void fill(char byte, size_t count)
    {
        FILE *fp = fopen(_path.c_str(), "wb");
        if (!fp)
            return;
        std::string chunk(65536, byte);
        while (count > 0)
        {
            size_t take = count < chunk.size() ? count : chunk.size();
            fwrite(chunk.data(), 1, take, fp);
            count -= take;
        }
        fclose(fp);
    }

private:
    std::string _dir;
    std::string _path;
};

const size_t BLOCK = 4 * 1024 * 1024;

} // namespace

TEST(hash, md5_matches_known_vectors)
{
    ScratchFile file;
    file.write("hello");
    CHECK_STR_EQ(computeMd5Hex(file.path()), "5d41402abc4b2a76b9719d911017c592");
    file.write("");
    CHECK_STR_EQ(computeMd5Hex(file.path()), "d41d8cd98f00b204e9800998ecf8427e");
}

TEST(hash, missing_file_hashes_to_empty_string)
{
    // "" means "could not read", which callers treat as "cannot compare".
    CHECK_STR_EQ(computeMd5Hex("/tmp/3dsync-does-not-exist-here"), "");
    CHECK_STR_EQ(computeDropboxHash("/tmp/3dsync-does-not-exist-here"), "");
}

TEST(hash, dropbox_hash_of_an_empty_file)
{
    // No blocks at all, so the outer digest is SHA-256 of no input.
    ScratchFile file;
    file.write("");
    CHECK_STR_EQ(computeDropboxHash(file.path()),
                 "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
}

TEST(hash, dropbox_hash_of_a_short_file)
{
    // SHA-256 of SHA-256("hello"), per the content-hash definition — note this
    // is not SHA-256("hello") itself, which is the mistake to guard against.
    ScratchFile file;
    file.write("hello");
    CHECK_STR_EQ(computeDropboxHash(file.path()),
                 "9595c9df90075148eb06860365df33584b75bff782a510c6cd4883a419833d50");
}

TEST(hash, dropbox_hash_at_block_boundaries)
{
    // Expected values come from the published definition of content_hash, so
    // these pin the block split itself, not just that the output changes.
    ScratchFile file;

    file.fill('a', BLOCK - 1); // one byte short of a block
    CHECK_STR_EQ(computeDropboxHash(file.path()),
                 "49c0a79929eacf8ccbe77f99ff043ca55f97000c68bccefc644791170969a3b2");

    file.fill('a', BLOCK); // exactly one block, no trailing partial block
    CHECK_STR_EQ(computeDropboxHash(file.path()),
                 "907a506cf5e706bda5c7a29b43c9c65d8344bd2fa2f22339b359c214812af5a1");

    file.fill('a', BLOCK + 1); // a second block holding a single byte
    CHECK_STR_EQ(computeDropboxHash(file.path()),
                 "5f858b62ccd88447586305aec6fd53c96747cfebf527cbba129a6dfed47d9624");

    file.fill('a', 2 * BLOCK); // two full blocks
    CHECK_STR_EQ(computeDropboxHash(file.path()),
                 "941162ca0d3fcd4e4b5bcaf179e3a156ae2b66b39e87993a7e626e61938ab2c9");
}

TEST(hash, dropbox_hash_is_stable_across_reads)
{
    ScratchFile file;
    file.fill('z', BLOCK + 12345);
    CHECK_STR_EQ(computeDropboxHash(file.path()), computeDropboxHash(file.path()));
}
