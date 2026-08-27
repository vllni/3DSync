#ifndef VERSION_H
#define VERSION_H

// ---------------------------------------------------------------------------
// The one place the version is written down.
// ---------------------------------------------------------------------------
// It used to come from `git describe` at build time, which meant the number on
// screen depended on the checkout rather than on the source: a shallow clone or
// a tarball had no tags and so no version, and the tag `describe` picked was
// not necessarily the release being built.  Now the source declares it, the
// Makefile reads the CIA/NRO metadata version out of the line below, and the
// release workflow refuses to publish a tag that disagrees with it.
//
// Releasing: bump APP_VERSION, commit, then tag that commit "v<version>" (the
// leading "v" is optional in the tag; the numbers must match exactly).  Keep
// the format "major.minor.micro" — the Makefile splits it on the dots.
// ---------------------------------------------------------------------------

#define APP_VERSION "0.5.1"

#endif
