#ifndef UTILS_SECRETS_H
#define UTILS_SECRETS_H

#include <stddef.h>
#include <stdint.h>
#include <string>

// ---------------------------------------------------------------------------
// Credentials sealed to the console
// ---------------------------------------------------------------------------
// 3DSync.ini is the only way to configure the app, so it holds passwords and
// OAuth tokens — and it sits on an SD card that is pulled, copied to PCs and
// attached to bug reports.  On first start, each secret in it is replaced by
//
//     Password=enc:v1:<base64 of nonce | AES-256-GCM ciphertext | tag>
//
// under a key derived from a console-unique value (CFGU_GenHashConsoleUnique,
// which comes from the console's LocalFriendCodeSeed and is not on the SD
// card).  The file is then useless on its own, but works exactly as before on
// this console.  The field's own "section=name" is the GCM associated data, so
// a sealed value cannot be moved to another field.
//
// What this does not protect against: anyone who can run homebrew on this
// very console can derive the same key.  It is about the card, not the box.
//
// Free of <3ds.h>: the console supplies the hash and the random bytes, so the
// host tests can drive everything here.
// ---------------------------------------------------------------------------

struct SealKey
{
    unsigned char bytes[32];
};

// HKDF-SHA256 over the console-unique hash.
SealKey deriveSealKey(uint64_t consoleHash);

// Fills buf with len random bytes; false if the source failed.
typedef bool (*SealRandom)(unsigned char *buf, size_t len);

// Whether [section] name= holds a credential.  Case-insensitive, as the INI
// reader is.  An explicit list: a field added later is sealed only once it is
// named here, which is the safe way round for a format other code reads.
bool isSealableSecret(const std::string &section, const std::string &name);

bool isSealedValue(const std::string &value);

bool sealValue(const SealKey &key, const std::string &section, const std::string &name,
               const std::string &plaintext, SealRandom random, std::string &out);

// False when the value is malformed, or was sealed on another console or for
// another field, or has been altered.
bool openSealedValue(const SealKey &key, const std::string &section, const std::string &name,
                     const std::string &sealed, std::string &out);

// Rewrite INI text with every plain secret sealed.  Everything else —
// comments, order, spacing, line endings, inline comments after a value — is
// kept byte for byte.  A secret is left alone when inih would not read it as
// one plain line: a key that repeats or continues on an indented line (inih
// joins those), or one whose sealed line would pass INI_MAX_LINE.
struct SealResult
{
    std::string text;
    int sealed;
    int skipped;
};
SealResult sealIniText(const std::string &text, const SealKey &key, SealRandom random);

#endif
