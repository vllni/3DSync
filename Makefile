TARGET := 3DS

NAME := 3DSync

BUILD_DIR := build
OUTPUT_DIR := output
INCLUDE_DIRS := include
SOURCE_DIRS := source
ROMFS_DIR := romfs

LIBRARY_DIRS += $(DEVKITPRO)/libctru $(DEVKITPRO)/portlibs/armv6k $(DEVKITPRO)/portlibs/3ds
LIBRARIES += smb2 curl mbedtls mbedx509 mbedcrypto z ctru m

EXTRA_OUTPUT_FILES := 

BUILD_FLAGS := -Wno-format-truncation -DINI_MAX_LINE=1024

# The version comes from source/version.h, not from `git describe`: the number
# on screen has to be a property of the source, so a shallow clone or a tarball
# build shows the same thing a tagged release does.  The release workflow checks
# the tag against this same line.
APP_VERSION := $(shell sed -n 's/^.*#define[[:space:]]*APP_VERSION[[:space:]]*"\([^"]*\)".*/\1/p' source/version.h)

ifeq ($(strip $(APP_VERSION)),)
    $(error Could not read APP_VERSION from source/version.h)
endif

VERSION_PARTS := $(subst ., ,$(APP_VERSION))

VERSION_MAJOR := $(word 1, $(VERSION_PARTS))
VERSION_MINOR := $(word 2, $(VERSION_PARTS))
VERSION_MICRO := $(word 3, $(VERSION_PARTS))

DESCRIPTION := Sync your saves
AUTHOR := michvllni

PRODUCT_CODE := CTR-K-SYNC
UNIQUE_ID := 0xF5555

BANNER_AUDIO := meta/audio_3ds.wav
BANNER_IMAGE := meta/banner_3ds.png
ICON := meta/icon_3ds.png

# INTERNAL #

include buildtools/make_base
