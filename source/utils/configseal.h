#ifndef UTILS_CONFIGSEAL_H
#define UTILS_CONFIGSEAL_H

#include <string>

#include "secrets.h"

// ---------------------------------------------------------------------------
// Sealing 3DSync.ini on the SD card
// ---------------------------------------------------------------------------
// sealIniText() decides what changes; this puts it on the card so that
//
//  - the plaintext does not survive as deleted-but-readable clusters: FAT
//    frees a replaced file's clusters without clearing them, and any undelete
//    tool reads them back.  So the original file is overwritten in place with
//    the sealed text rather than swapped out by rename; and
//  - an interrupted write cannot lose the configuration.  The sealed text is
//    written to "<ini>.3dstmp" first and renamed to "<ini>.sealed" once
//    complete; only then is the original overwritten.  A ".sealed" left over
//    means the overwrite may not have finished, and recoverConfigSeal() redoes
//    it; a ".3dstmp" left over was never complete, and is dropped.
//
// The SD card's own flash controller may still keep an old copy of a block
// it has remapped, out of reach of anything that reads the card normally.
// ---------------------------------------------------------------------------

// Finish or undo a seal that was interrupted.  Call before reading the INI.
void recoverConfigSeal(const std::string &path);

// Seal the plain secrets in the file at path.  Returns how many were sealed
// (0 when there was nothing to do), or -1 if the file could not be rewritten,
// in which case it is left as it was.
int sealConfigFile(const std::string &path, const SealKey &key, SealRandom random);

#endif
