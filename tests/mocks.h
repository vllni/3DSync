#ifndef TESTS_MOCKS_H
#define TESTS_MOCKS_H

#include <map>
#include <stdio.h>
#include <string>
#include <vector>

#include "../source/sync/syncengine.h"

// ---------------------------------------------------------------------------
// MockRemote — a SyncProvider that keeps its files in memory
// ---------------------------------------------------------------------------
// SyncProvider is the one seam every backend shares, so mocking there exercises
// the engine against all of them at once: the decision table cannot tell a mock
// from Drive, SMB, FTP, WebDAV or Dropbox.  (An HTTP-level mock could not do
// this — SMB does not speak HTTP at all.)
//
// Failures are scripted rather than simulated, because what matters to the
// engine is only whether a transfer reported success.
// ---------------------------------------------------------------------------
class MockRemote : public SyncProvider
{
public:
    struct RemoteFile
    {
        std::string content;
        std::string tag;
        std::string id;
    };

    // Remote contents, keyed by relPath ("/Game/001.sav").
    std::map<std::string, RemoteFile> files;

    // Scripted behaviour.
    bool failList = false;
    bool failUploads = false;
    bool failDownloads = false;
    bool fatalOnList = false;
    bool experimental = true;
    // With contentTags on, tags are derived from file contents, the way Drive's
    // MD5 and Dropbox's content_hash are — which is what makes localTag() work.
    bool contentTags = false;
    std::string prefix = "mock";

    // Recorded calls, for assertions.
    int listCalls = 0;
    std::vector<std::string> uploaded;   // relPaths, in call order
    std::vector<std::string> downloaded;
    std::vector<std::string> ensuredRoots;

    // --- SyncProvider ---
    const char *name() const override { return "Mock"; }
    std::string manifestPrefix() const override { return prefix; }
    bool isExperimental() const override { return experimental; }
    bool connect() override { return true; }
    bool hasFatalError() const override { return _fatalError; }

    std::string ensureRoot(const std::string &remoteName) override
    {
        ensuredRoots.push_back(remoteName);
        return "/root/" + remoteName;
    }

    bool list(const std::string &root, std::map<std::string, RemoteFileInfo> &out) override
    {
        (void)root;
        listCalls++;
        if (fatalOnList)
        {
            _fatalError = true;
            return false;
        }
        if (failList)
            return false;

        for (auto &entry : files)
        {
            RemoteFileInfo info;
            info.relPath = entry.first;
            info.tag = entry.second.tag;
            info.id = entry.second.id.empty() ? ("id" + entry.first) : entry.second.id;
            out[entry.first] = info;
        }
        return true;
    }

    bool download(const RemoteFileInfo &file, const std::string &localPath) override
    {
        downloaded.push_back(file.relPath);
        if (failDownloads)
            return false;

        auto it = files.find(file.relPath);
        if (it == files.end())
            return false;

        FILE *fp = fopen(localPath.c_str(), "wb");
        if (!fp)
            return false;
        fwrite(it->second.content.data(), 1, it->second.content.size(), fp);
        fclose(fp);
        return true;
    }

    bool upload(const std::string &root, const std::string &relPath,
                const std::string &localPath, const RemoteFileInfo *existing,
                std::string &outTag, std::string &outId) override
    {
        (void)root;
        (void)existing;
        uploaded.push_back(relPath);
        if (failUploads)
            return false;

        std::string content = readLocal(localPath);
        RemoteFile file;
        file.content = content;
        file.tag = tagForContent(content);
        file.id = "id" + relPath;
        files[relPath] = file;

        outTag = file.tag;
        outId = file.id;
        return true;
    }

    std::string localTag(const std::string &localPath) override
    {
        if (!contentTags)
            return "";
        return tagForContent(readLocal(localPath));
    }

    // Tag a remote file as if it had been changed on the server.
    void setRemote(const std::string &relPath, const std::string &content)
    {
        RemoteFile file;
        file.content = content;
        file.tag = tagForContent(content);
        file.id = "id" + relPath;
        files[relPath] = file;
    }

    // The tag a remote file currently reports.
    std::string tagOf(const std::string &relPath)
    {
        auto it = files.find(relPath);
        return it == files.end() ? std::string() : it->second.tag;
    }

    static std::string readLocal(const std::string &path)
    {
        FILE *fp = fopen(path.c_str(), "rb");
        if (!fp)
            return "";
        std::string out;
        char buf[4096];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
            out.append(buf, n);
        fclose(fp);
        return out;
    }

private:
    bool _fatalError = false;

    // Cheap content-derived tag; only its determinism matters.
    static std::string tagForContent(const std::string &content)
    {
        unsigned long hash = 5381;
        for (unsigned char c : content)
            hash = ((hash << 5) + hash) + c;
        char buf[64];
        snprintf(buf, sizeof(buf), "c%lu:%zu", hash, content.size());
        return buf;
    }
};

// ---------------------------------------------------------------------------
// MockUi — scripted answers instead of button presses
// ---------------------------------------------------------------------------
class MockUi : public SyncUi
{
public:
    // Consumed in order; the last answer repeats once exhausted.
    std::vector<ConflictChoice> answers;
    // Cancel once cancelRequested() has been polled this many times (-1: never).
    int cancelAfterPolls = -1;

    int polls = 0;
    std::vector<std::string> conflictsRaised;
    bool cancelled = false;

    bool cancelRequested() override
    {
        polls++;
        if (cancelAfterPolls >= 0 && polls > cancelAfterPolls)
            cancelled = true;
        return cancelled;
    }

    void requestCancel() override { cancelled = true; }

    ConflictChoice resolveConflict(const std::string &localPath) override
    {
        conflictsRaised.push_back(localPath);
        if (answers.empty())
            return CONFLICT_SKIP;
        size_t index = _answerIndex < answers.size() ? _answerIndex : answers.size() - 1;
        _answerIndex++;
        return answers[index];
    }

private:
    size_t _answerIndex = 0;
};

// ---------------------------------------------------------------------------
// TempDir — a scratch directory for the local side of a sync
// ---------------------------------------------------------------------------
class TempDir
{
public:
    TempDir();
    ~TempDir();

    const std::string &path() const { return _path; }

    // Write a local file (creating parent directories) and optionally stamp it
    // with a fixed mtime, so a test can hold the timestamp still while the
    // contents change — the case FAT and some emulators produce.
    void write(const std::string &relPath, const std::string &content,
               long long mtime = 0);
    std::string read(const std::string &relPath) const;
    bool exists(const std::string &relPath) const;
    long long mtimeOf(const std::string &relPath) const;

private:
    std::string _path;
};

#endif
