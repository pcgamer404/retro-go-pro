/* ScummVM - Graphic Adventure Engine
 *
 * ScummVM is the legal property of its developers, whose names
 * are too numerous to list here. Please refer to the COPYRIGHT
 * file distributed with this source distribution.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */

#ifndef COMMON_UNZIP_H
#define COMMON_UNZIP_H

#include "common/str.h"

namespace Common { class SeekableReadStream; }

/* Opaque low-level ZIP handle. These APIs intentionally remain outside the
 * Common namespace, matching the implementation in unzip.cpp. */
typedef void *unzFile;

unzFile unzOpenNoIndex(Common::SeekableReadStream *stream);
int unzClose(unzFile file);
int unzGoToFirstFile(unzFile file);
int unzGoToNextFile(unzFile file);
int unzGetCurrentFileInfo(unzFile file, void *pfile_info, char *szFileName,
                          unsigned fileNameBufferSize, void *extraField,
                          unsigned extraFieldBufferSize, char *szComment,
                          unsigned commentBufferSize);
int unzOpenCurrentFile(unzFile file);
int unzReadCurrentFile(unzFile file, void *buf, unsigned len);
int unzCloseCurrentFile(unzFile file);

namespace Common {

class Archive;
class FSNode;
class SeekableReadStream;

/**
 * This factory method creates an Archive instance corresponding to the content
 * of the ZIP compressed file with the given name.
 *
 * May return 0 in case of a failure.
 */
Archive *makeZipArchive(const String &name);

/**
 * This factory method creates an Archive instance corresponding to the content
 * of the ZIP compressed file with the given name.
 *
 * May return 0 in case of a failure.
 */
Archive *makeZipArchive(const FSNode &node);

/**
 * This factory method creates an Archive instance corresponding to the content
 * of the given ZIP compressed datastream.
 * This takes ownership of the stream, in particular, it is deleted when the
 * ZipArchive is deleted.
 *
 * May return 0 in case of a failure. In this case stream will still be deleted.
 */
Archive *makeZipArchive(SeekableReadStream *stream);

} // End of namespace Common

#endif
