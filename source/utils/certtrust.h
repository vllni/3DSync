#ifndef UTILS_CERTTRUST_H
#define UTILS_CERTTRUST_H

#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// Which server certificates a WebDAV or FTP remote trusts
// ---------------------------------------------------------------------------
// By default a certificate must chain to one of the public roots in
// romfs/cacert.pem and name the host.  A NAS with a certificate of its own
// fails that, so its section can say what to trust instead:
//
//   PinnedKey=   the SHA-256 of the server's public key (SubjectPublicKeyInfo),
//                or of its whole certificate.  Exactly that key is accepted,
//                whoever signed it and whatever name it carries.  Survives a
//                renewal that keeps the key; a certificate hash does not.
//   CACert=      the certificate(s) of a home or router CA, as base64 DER on
//                one line.  The server's chain is checked against these alone,
//                its name still against the host.
//
// Free of mbedTLS and libcurl, so the parsing is tested on the host; Curl
// does the checking.
// ---------------------------------------------------------------------------
struct TlsTrust
{
    // Check the certificate at all.  Off only for VerifyTLS=false.
    bool verify = true;
    // Raw 32-byte SHA-256 digests; any one matching is enough.
    std::vector<std::string> pinnedKeys;
    // DER certificates that replace the public roots for this remote.
    std::vector<std::string> caCerts;
};

// PinnedKey=: hashes separated by commas, each 64 hex digits (any case,
// colons and spaces allowed between them, as browsers show a fingerprint) or
// "sha256//" and base64 (curl's and openssl's form).  False, with out
// emptied, if any of them is neither.
bool parsePinnedKeys(const std::string &value, std::vector<std::string> &out);

// CACert=: base64 DER certificates separated by commas.  The BEGIN and END
// lines of a PEM pasted onto the line are allowed and separate too.  Each
// must decode to one DER structure exactly; false, with out emptied, if any
// does not, or if there is none.
bool parseCaCerts(const std::string &value, std::vector<std::string> &out);

// The host of a URL ("https://user:pw@192.168.1.5:8443/dav/" -> "192.168.1.5"),
// as libcurl connects to it.  "" when there is none.
std::string urlHost(const std::string &url);

// Whether a DER certificate lists this dotted IPv4 address among its subject
// alternative names.  mbedTLS 2.28 only matches the host against DNS names,
// so a certificate made for a server reached by its address, which lists it
// as an iPAddress entry, fails the name check without this.  False for a
// host that is no IPv4 address (the 3DS has no IPv6) or a certificate it
// cannot read.
bool certificateListsIp(const std::string &der, const std::string &host);

// A digest as two lines of colon-separated upper-case hex, 16 bytes each, to
// print under the log's 50 columns.  parsePinnedKeys() reads them back
// joined with or without the colon between.
std::vector<std::string> fingerprintLines(const std::string &digest);

#endif
