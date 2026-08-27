#include "smbremote.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <vector>

#include <3ds.h>

// smb2.h defines the constants and typedefs libsmb2.h uses in its prototypes,
// so it has to come first.
#include <smb2/smb2.h>
#include <smb2/libsmb2.h>

#include "../utils/fsutil.h"

// Read/write in chunks, clamped to what the server negotiated.
static const uint32_t SMB_CHUNK = 65536;

SmbRemote::SmbRemote(const std::string &server, const std::string &share,
                     const std::string &user, const std::string &password,
                     const std::string &domain, const std::string &basePath)
    : _server(server), _share(share), _user(user), _password(password),
      _domain(domain), _basePath(_normalize(basePath)), _smb2(NULL),
      _connected(false), _fatalError(false)
{
}

SmbRemote::~SmbRemote()
{
    if (_smb2)
    {
        if (_connected)
            smb2_disconnect_share(_smb2);
        smb2_destroy_context(_smb2);
    }
}

std::string SmbRemote::manifestPrefix() const
{
    return "smb://" + _server + "/" + _share;
}

bool SmbRemote::hasFatalError() const { return _fatalError; }

bool SmbRemote::connect()
{
    _smb2 = smb2_init_context();
    if (!_smb2)
    {
        printf("SMB: out of memory\n");
        _fatalError = true;
        return false;
    }

    smb2_set_security_mode(_smb2, SMB2_NEGOTIATE_SIGNING_ENABLED);
    smb2_set_timeout(_smb2, 30);
    if (!_password.empty())
        smb2_set_password(_smb2, _password.c_str());
    if (!_domain.empty())
        smb2_set_domain(_smb2, _domain.c_str());

    int res = smb2_connect_share(_smb2, _server.c_str(), _share.c_str(),
                                 _user.empty() ? NULL : _user.c_str());
    if (res != 0)
    {
        printf(CONSOLE_RED "SMB: cannot connect to \\\\%s\\%s" CONSOLE_RESET "\n",
               _server.c_str(), _share.c_str());
        printf("  %s\n", smb2_get_error(_smb2));
        printf("  Check the server address, share name and credentials.\n");
        _fatalError = true; // wrong host or credentials — retrying will not help
        return false;
    }

    _connected = true;
    return true;
}

std::string SmbRemote::_normalize(const std::string &path)
{
    std::string out;
    for (size_t i = 0; i < path.size(); i++)
    {
        char c = (path[i] == '\\') ? '/' : path[i];
        // Collapse repeated separators; libsmb2 wants clean components.
        if (c == '/' && (out.empty() || out[out.size() - 1] == '/'))
            continue;
        out += c;
    }
    while (!out.empty() && out[out.size() - 1] == '/')
        out.erase(out.size() - 1);
    return out;
}

std::string SmbRemote::_tagFor(const struct smb2_stat_64 &st)
{
    char buf[48];
    snprintf(buf, sizeof(buf), "%llu:%llu",
             (unsigned long long)st.smb2_size, (unsigned long long)st.smb2_mtime);
    return buf;
}

bool SmbRemote::_mkdirs(const std::string &dirPath)
{
    if (dirPath.empty())
        return true;

    size_t pos = 0;
    while (pos != std::string::npos)
    {
        pos = dirPath.find('/', pos + 1);
        std::string part = (pos == std::string::npos) ? dirPath : dirPath.substr(0, pos);

        struct smb2_stat_64 st = {};
        if (smb2_stat(_smb2, part.c_str(), &st) == 0)
        {
            if (st.smb2_type == SMB2_TYPE_DIRECTORY)
                continue;
            printf("SMB: %s exists but is not a directory\n", part.c_str());
            return false;
        }
        if (smb2_mkdir(_smb2, part.c_str()) != 0)
        {
            // Another sync may have created it between the stat and the mkdir.
            if (smb2_stat(_smb2, part.c_str(), &st) == 0 &&
                st.smb2_type == SMB2_TYPE_DIRECTORY)
                continue;
            printf("SMB: cannot create %s: %s\n", part.c_str(), smb2_get_error(_smb2));
            return false;
        }
    }
    return true;
}

std::string SmbRemote::ensureRoot(const std::string &remoteName)
{
    std::string root = _normalize(_basePath.empty() ? remoteName
                                                    : _basePath + "/" + remoteName);
    if (!_mkdirs(root))
        return "";
    return root;
}

bool SmbRemote::_listInto(const std::string &dirPath, const std::string &prefix,
                          std::map<std::string, RemoteFileInfo> &out)
{
    struct smb2dir *dir = smb2_opendir(_smb2, dirPath.c_str());
    if (!dir)
    {
        printf("SMB: cannot list %s: %s\n", dirPath.c_str(), smb2_get_error(_smb2));
        return false;
    }

    // Collect first, recurse after closing: libsmb2 keeps per-directory state
    // and nesting opendir calls would hold a handle open for every level.
    std::vector<std::string> subdirs;
    struct smb2dirent *ent;
    while ((ent = smb2_readdir(_smb2, dir)) != NULL)
    {
        std::string entryName(ent->name);
        if (entryName == "." || entryName == "..")
            continue;

        std::string childPath = dirPath.empty() ? entryName : dirPath + "/" + entryName;
        std::string childRel = prefix + "/" + entryName;

        if (ent->st.smb2_type == SMB2_TYPE_DIRECTORY)
        {
            subdirs.push_back(entryName);
        }
        else if (ent->st.smb2_type == SMB2_TYPE_FILE)
        {
            // Skip our own interrupted transfers.
            if (childRel.size() > 7 &&
                childRel.compare(childRel.size() - 7, 7, ".3dstmp") == 0)
                continue;

            RemoteFileInfo info;
            info.id = childPath;
            info.relPath = childRel;
            info.tag = _tagFor(ent->st);
            out[childRel] = info;
        }
    }
    smb2_closedir(_smb2, dir);

    for (auto &sub : subdirs)
    {
        std::string childPath = dirPath.empty() ? sub : dirPath + "/" + sub;
        if (!_listInto(childPath, prefix + "/" + sub, out))
            return false;
    }
    return true;
}

bool SmbRemote::list(const std::string &root,
                     std::map<std::string, RemoteFileInfo> &out)
{
    return _listInto(root, "", out);
}

bool SmbRemote::download(const RemoteFileInfo &file, const std::string &localPath)
{
    struct smb2fh *fh = smb2_open(_smb2, file.id.c_str(), O_RDONLY);
    if (!fh)
    {
        printf("SMB: cannot open %s: %s\n", file.id.c_str(), smb2_get_error(_smb2));
        return false;
    }

    std::string tmpPath;
    FILE *fp = openTempFor(localPath, tmpPath);
    if (!fp)
    {
        smb2_close(_smb2, fh);
        return false;
    }

    uint32_t chunk = smb2_get_max_read_size(_smb2);
    if (chunk == 0 || chunk > SMB_CHUNK)
        chunk = SMB_CHUNK;

    std::vector<uint8_t> buf(chunk);
    uint64_t offset = 0;
    bool ok = true;

    while (true)
    {
        int n = smb2_pread(_smb2, fh, &buf[0], chunk, offset);
        if (n == 0)
            break; // EOF
        if (n < 0)
        {
            printf("SMB: read failed on %s: %s\n", file.id.c_str(), smb2_get_error(_smb2));
            ok = false;
            break;
        }
        if (fwrite(&buf[0], 1, (size_t)n, fp) != (size_t)n)
        {
            printf("SMB: cannot write %s (SD card full?)\n", tmpPath.c_str());
            ok = false;
            break;
        }
        offset += (uint64_t)n;
    }

    smb2_close(_smb2, fh);
    fclose(fp);

    if (!ok)
    {
        remove(tmpPath.c_str());
        return false;
    }
    return replaceLocalFile(tmpPath, localPath);
}

bool SmbRemote::upload(const std::string &root, const std::string &relPath,
                       const std::string &localPath, const RemoteFileInfo *existing,
                       std::string &outTag, std::string &outId)
{
    (void)existing;

    std::string remotePath = _normalize(root + relPath);
    size_t slash = remotePath.rfind('/');
    if (slash != std::string::npos && !_mkdirs(remotePath.substr(0, slash)))
        return false;

    FILE *fp = fopen(localPath.c_str(), "rb");
    if (!fp)
    {
        printf("SMB: cannot read %s\n", localPath.c_str());
        return false;
    }

    // Write to a temp name and swap it in, so a failed transfer never leaves a
    // truncated save on the server.
    std::string tmpRemote = remotePath + ".3dstmp";
    struct smb2fh *fh = smb2_open(_smb2, tmpRemote.c_str(), O_WRONLY | O_CREAT | O_TRUNC);
    if (!fh)
    {
        printf("SMB: cannot create %s: %s\n", tmpRemote.c_str(), smb2_get_error(_smb2));
        fclose(fp);
        return false;
    }

    uint32_t chunk = smb2_get_max_write_size(_smb2);
    if (chunk == 0 || chunk > SMB_CHUNK)
        chunk = SMB_CHUNK;

    std::vector<uint8_t> buf(chunk);
    uint64_t offset = 0;
    bool ok = true;
    size_t n;

    while ((n = fread(&buf[0], 1, chunk, fp)) > 0)
    {
        size_t written = 0;
        while (written < n)
        {
            int w = smb2_pwrite(_smb2, fh, &buf[written], (uint32_t)(n - written),
                                offset + written);
            if (w <= 0)
            {
                printf("SMB: write failed on %s: %s\n", tmpRemote.c_str(),
                       smb2_get_error(_smb2));
                ok = false;
                break;
            }
            written += (size_t)w;
        }
        if (!ok)
            break;
        offset += (uint64_t)written;
    }

    smb2_close(_smb2, fh);
    fclose(fp);

    if (!ok)
    {
        smb2_unlink(_smb2, tmpRemote.c_str());
        return false;
    }

    // smb2_rename() never replaces an existing target (libsmb2 sends
    // ReplaceIfExists=0), so the old file has to go first.
    smb2_unlink(_smb2, remotePath.c_str());
    if (smb2_rename(_smb2, tmpRemote.c_str(), remotePath.c_str()) != 0)
    {
        printf("SMB: cannot rename %s into place: %s\n", tmpRemote.c_str(),
               smb2_get_error(_smb2));
        smb2_unlink(_smb2, tmpRemote.c_str());
        return false;
    }

    struct smb2_stat_64 st = {};
    if (smb2_stat(_smb2, remotePath.c_str(), &st) != 0)
    {
        printf("SMB: uploaded %s but cannot stat it back\n", remotePath.c_str());
        return false;
    }

    outTag = _tagFor(st);
    outId = remotePath;
    return true;
}
