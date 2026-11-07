// The local side of a transfer: a download may only replace a save once all of
// it is on the card, a swap that fails must give the old copy back, and what
// an interrupted transfer leaves behind must never be synced as a save.

#include <stdio.h>
#include <sys/stat.h>

#include <algorithm>

#include "framework.h"
#include "mocks.h"

#include "../source/utils/fsutil.h"

namespace
{

// A download's temp file holding contents, opened for finishDownload().
FILE *tempWith(const TempDir &dir, const std::string &relPath, const std::string &contents,
               std::string &tmpPath)
{
    FILE *fp = openTempFor(dir.path() + relPath, tmpPath);
    if (fp)
        fwrite(contents.data(), 1, contents.size(), fp);
    return fp;
}

std::string fakeHash(const std::string &path)
{
    FILE *fp = fopen(path.c_str(), "rb");
    if (!fp)
        return "";
    std::string out;
    char buf[64];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
        out.append(buf, n);
    fclose(fp);
    return "hash:" + out;
}

std::vector<std::string> sorted(std::vector<std::string> v)
{
    std::sort(v.begin(), v.end());
    return v;
}

} // namespace

TEST(fsutil, a_complete_download_replaces_the_file)
{
    TempDir dir;
    dir.write("/a.sav", "old");
    std::string tmp;
    FILE *fp = tempWith(dir, "/a.sav", "new!", tmp);
    CHECK(fp != NULL);

    DownloadCheck check;
    check.received = 4;
    check.hashOf = fakeHash;
    check.expectedHash = "hash:new!";
    CHECK(finishDownload(fp, tmp, dir.path() + "/a.sav", check));
    CHECK_STR_EQ(dir.read("/a.sav"), "new!");
    CHECK(!dir.exists("/a.sav.3dstmp"));
    CHECK(!dir.exists("/a.sav.3dsbak"));
}

TEST(fsutil, a_download_short_of_what_was_received_keeps_the_old_file)
{
    TempDir dir;
    dir.write("/a.sav", "old");
    std::string tmp;
    FILE *fp = tempWith(dir, "/a.sav", "ne", tmp);

    DownloadCheck check;
    check.received = 4; // two bytes never reached the card
    CHECK(!finishDownload(fp, tmp, dir.path() + "/a.sav", check));
    CHECK_STR_EQ(dir.read("/a.sav"), "old");
    CHECK(!dir.exists("/a.sav.3dstmp"));
}

TEST(fsutil, a_download_with_the_wrong_hash_keeps_the_old_file)
{
    TempDir dir;
    dir.write("/a.sav", "old");
    std::string tmp;
    FILE *fp = tempWith(dir, "/a.sav", "bad!", tmp);

    DownloadCheck check;
    check.received = 4;
    check.hashOf = fakeHash;
    check.expectedHash = "hash:new!";
    CHECK(!finishDownload(fp, tmp, dir.path() + "/a.sav", check));
    CHECK_STR_EQ(dir.read("/a.sav"), "old");
    CHECK(!dir.exists("/a.sav.3dstmp"));
}

TEST(fsutil, no_published_hash_skips_the_content_check)
{
    TempDir dir;
    std::string tmp;
    FILE *fp = tempWith(dir, "/new.sav", "data", tmp);

    DownloadCheck check;
    check.received = 4;
    check.hashOf = fakeHash; // the remote listed no hash for this file
    CHECK(finishDownload(fp, tmp, dir.path() + "/new.sav", check));
    CHECK_STR_EQ(dir.read("/new.sav"), "data");
}

TEST(fsutil, a_write_that_fails_at_close_keeps_the_old_file)
{
    TempDir dir;
    dir.write("/a.sav", "old");
    std::string tmp;
    FILE *fp = tempWith(dir, "/a.sav", "", tmp);
    fclose(fp);
    // /dev/full accepts writes into the buffer and refuses the flush, which
    // is what a full SD card does at the last fclose().
    fp = fopen("/dev/full", "w");
    CHECK(fp != NULL);
    if (!fp)
        return;
    fputs("new", fp);

    CHECK(!finishDownload(fp, tmp, dir.path() + "/a.sav", DownloadCheck()));
    CHECK_STR_EQ(dir.read("/a.sav"), "old");
    CHECK(!dir.exists("/a.sav.3dstmp"));
}

TEST(fsutil, a_failed_swap_restores_the_backup)
{
    TempDir dir;
    dir.write("/a.sav", "old");
    // No temp file to rename in: the swap fails after the original was parked.
    CHECK(!replaceLocalFile(dir.path() + "/missing.3dstmp", dir.path() + "/a.sav"));
    CHECK_STR_EQ(dir.read("/a.sav"), "old");
    CHECK(!dir.exists("/a.sav.3dsbak"));
}

TEST(fsutil, listing_skips_transfer_leftovers)
{
    TempDir dir;
    dir.write("/a.sav", "a");
    dir.write("/a.sav.3dstmp", "half");
    dir.write("/a.sav.3dsbak", "older");
    dir.write("/sub/b.sav", "b");
    dir.write("/sub/b.sav.3dstmp", "half");

    std::vector<std::string> files = sorted(listLocalFiles(dir.path(), true));
    CHECK_EQ(files.size(), (size_t)2);
    if (files.size() == 2)
    {
        CHECK_STR_EQ(files[0], "/a.sav");
        CHECK_STR_EQ(files[1], "/sub/b.sav");
    }
    // A backup beside its original is left for the user, not deleted.
    CHECK(dir.exists("/a.sav.3dsbak"));
}

TEST(fsutil, a_backup_without_its_original_is_restored)
{
    TempDir dir;
    // replaceLocalFile() parked the save and stopped before the new one
    // arrived: the backup is the last good copy.
    dir.write("/a.sav.3dsbak", "last good");
    dir.write("/a.sav.3dstmp", "half");

    std::vector<std::string> files = listLocalFiles(dir.path(), true);
    CHECK_EQ(files.size(), (size_t)1);
    if (files.size() == 1)
        CHECK_STR_EQ(files[0], "/a.sav");
    CHECK_STR_EQ(dir.read("/a.sav"), "last good");
    CHECK(!dir.exists("/a.sav.3dsbak"));
}

TEST(fsutil, a_shallow_listing_leaves_subfolders_out)
{
    TempDir dir;
    dir.write("/a.sav", "a");
    dir.write("/sub/b.sav", "b");

    std::vector<std::string> files = listLocalFiles(dir.path(), false);
    CHECK_EQ(files.size(), (size_t)1);
    if (files.size() == 1)
        CHECK_STR_EQ(files[0], "/a.sav");
}

TEST(fsutil, a_missing_folder_lists_nothing)
{
    TempDir dir;
    CHECK_EQ(listLocalFiles(dir.path() + "/nope", true).size(), (size_t)0);
}
