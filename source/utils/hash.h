#ifndef UTILS_HASH_H
#define UTILS_HASH_H

#include <string>

// Content hashes used as change tags.  Only two remotes publish one that can be
// recomputed locally, which is what lets those two notice a file whose
// timestamp never moved.

// MD5 of a local file as a lowercase hex string, "" on error.
std::string computeMd5Hex(const std::string &path);

// Dropbox content hash of a local file as a lowercase hex string, "" on error.
// Dropbox does not publish an MD5: its content_hash is the SHA-256 of the
// concatenated SHA-256 digests of each 4 MiB block of the file.
// https://www.dropbox.com/developers/reference/content-hash
std::string computeDropboxHash(const std::string &path);

#endif
