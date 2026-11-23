/***************************************************************************
                            qgsziputils.cpp
                          ---------------------
    begin                : Jul 2017
    copyright            : (C) 2017 by Paul Blottiere
    email                : paul.blottiere@oslandia.com
 ***************************************************************************
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 ***************************************************************************/

#include "qgsziputils.h"

#include <cstring>
#include <fstream>
#include <iostream>
#include <vector>
#include <zip.h>
#include <zlib.h>

#include "qgslogger.h"
#include "qgsmessagelog.h"

#include <QDir>
#include <QFileInfo>
#include <QString>

using namespace Qt::StringLiterals;

bool QgsZipUtils::isZipFile( const QString &filename )
{
  return QFileInfo( filename ).suffix().compare( "qgz"_L1, Qt::CaseInsensitive ) == 0;
}

bool QgsZipUtils::unzip( const QString &zipFilename, const QString &dir, QStringList &files, bool checkConsistency )
{
  files.clear();

  if ( !QFileInfo::exists( zipFilename ) )
  {
    QgsMessageLog::logMessage( QObject::tr( "Error zip file does not exist: '%1'" ).arg( zipFilename ) );
    return false;
  }
  else if ( zipFilename.isEmpty() )
  {
    QgsMessageLog::logMessage( QObject::tr( "Error zip filename is empty" ) );
    return false;
  }
  else if ( !QDir( dir ).exists( dir ) )
  {
    QgsMessageLog::logMessage( QObject::tr( "Error output dir does not exist: '%1'" ).arg( dir ) );
    return false;
  }
  else if ( !QFileInfo( dir ).isDir() )
  {
    QgsMessageLog::logMessage( QObject::tr( "Error output dir is not a directory: '%1'" ).arg( dir ) );
    return false;
  }
  else if ( !QFileInfo( dir ).isWritable() )
  {
    QgsMessageLog::logMessage( QObject::tr( "Error output dir is not writable: '%1'" ).arg( dir ) );
    return false;
  }

  int rc = 0;
  const QByteArray fileNamePtr = zipFilename.toUtf8();
  struct zip *z = zip_open( fileNamePtr.constData(), checkConsistency ? ZIP_CHECKCONS : 0, &rc );

  if ( rc == ZIP_ER_OK && z )
  {
    const int count = zip_get_num_entries( z, ZIP_FL_UNCHANGED );
    if ( count != -1 )
    {
      struct zip_stat stat;

      for ( int i = 0; i < count; i++ )
      {
        zip_stat_index( z, i, 0, &stat );
        const size_t len = stat.size;

        struct zip_file *file = zip_fopen_index( z, i, 0 );
        const std::unique_ptr< char[] > buf( new char[len] );
        if ( zip_fread( file, buf.get(), len ) != -1 )
        {
          const QString fileName( stat.name );
          if ( fileName.endsWith( "/" ) )
          {
            continue;
          }

          const QFileInfo newFile( QDir( dir ), fileName );

          if ( !QString( QDir::cleanPath( newFile.absolutePath() ) + u"/"_s ).startsWith( QDir( dir ).absolutePath() + u"/"_s ) )
          {
            QgsMessageLog::logMessage( QObject::tr( "Skipped file %1 outside of the directory %2" ).arg( newFile.absoluteFilePath(), QDir( dir ).absolutePath() ) );
            continue;
          }

          // Create path for a new file if it does not exist.
          if ( !newFile.absoluteDir().exists() )
          {
            if ( !QDir( dir ).mkpath( newFile.absolutePath() ) )
              QgsMessageLog::logMessage( QObject::tr( "Failed to create a subdirectory %1/%2" ).arg( dir ).arg( fileName ) );
          }

          QFile outFile( newFile.absoluteFilePath() );
          if ( !outFile.open( QIODevice::WriteOnly | QIODevice::Truncate ) )
          {
            QgsMessageLog::logMessage( QObject::tr( "Could not write to %1" ).arg( newFile.absoluteFilePath() ) );
          }
          else
          {
            outFile.write( buf.get(), len );
          }
          zip_fclose( file );
          files.append( newFile.absoluteFilePath() );
        }
        else
        {
          zip_fclose( file );
          QgsMessageLog::logMessage( QObject::tr( "Error reading file: '%1'" ).arg( zip_strerror( z ) ) );
          return false;
        }
      }
    }
    else
    {
      zip_close( z );
      QgsMessageLog::logMessage( QObject::tr( "Error getting files: '%1'" ).arg( zip_strerror( z ) ) );
      return false;
    }

    zip_close( z );
  }
  else
  {
    QgsMessageLog::logMessage( QObject::tr( "Error opening zip archive: '%1' (Error code: %2)" ).arg( z ? zip_strerror( z ) : zipFilename ).arg( rc ) );
    return false;
  }

  return true;
}

namespace
{
  // Returns TRUE if the entry at index in archive holds exactly the bytes of the file at path.
  bool entryEqualsFile( struct zip *archive, zip_uint64_t index, const QString &path )
  {
    QFile file( path );
    if ( !file.open( QIODevice::ReadOnly ) )
      return false;

    struct zip_file *entry = zip_fopen_index( archive, index, 0 );
    if ( !entry )
      return false;

    constexpr qint64 CHUNK_SIZE = 1 << 16;
    std::vector<char> fileBuffer( CHUNK_SIZE );
    std::vector<char> entryBuffer( CHUNK_SIZE );
    bool equal = true;
    while ( equal )
    {
      const qint64 fileRead = file.read( fileBuffer.data(), CHUNK_SIZE );
      // zip_fread() also verifies the CRC of the entry once the end is reached.
      const zip_int64_t entryRead = zip_fread( entry, entryBuffer.data(), CHUNK_SIZE );
      equal = fileRead >= 0 && fileRead == entryRead && std::memcmp( fileBuffer.data(), entryBuffer.data(), static_cast<size_t>( fileRead ) ) == 0;
      if ( fileRead <= 0 )
        break;
    }
    zip_fclose( entry );
    return equal;
  }

  // Returns a source which copies the compressed data of the entry named after the file from
  // reuseArchive, or nullptr if reuseArchive has no such entry with content identical to the file.
  struct zip_source *reusedEntrySource( struct zip *archive, struct zip *reuseArchive, const QFileInfo &fileInfo )
  {
    if ( !reuseArchive )
      return nullptr;

    const QByteArray name = fileInfo.fileName().toUtf8();
    const zip_int64_t index = zip_name_locate( reuseArchive, name.constData(), 0 );
    if ( index < 0 )
      return nullptr;

    struct zip_stat stat;
    zip_stat_init( &stat );
    if ( zip_stat_index( reuseArchive, index, 0, &stat ) != 0 || !( stat.valid & ZIP_STAT_SIZE ) || stat.size != static_cast<zip_uint64_t>( fileInfo.size() ) )
      return nullptr;

    if ( !entryEqualsFile( reuseArchive, index, fileInfo.absoluteFilePath() ) )
      return nullptr;

    // With the compressed data as source, libzip writes the entry with its original compression method and without recompressing it.
#if LIBZIP_VERSION_MAJOR > 1 || ( LIBZIP_VERSION_MAJOR == 1 && LIBZIP_VERSION_MINOR >= 10 )
    return zip_source_zip_file( archive, reuseArchive, index, ZIP_FL_COMPRESSED, 0, -1, nullptr );
#else
    return zip_source_zip( archive, reuseArchive, index, ZIP_FL_COMPRESSED, 0, -1 );
#endif
  }
} // namespace

bool QgsZipUtils::zip( const QString &zipFilename, const QStringList &files, bool overwrite, const QString &reuseFrom )
{
  if ( zipFilename.isEmpty() )
  {
    QgsMessageLog::logMessage( QObject::tr( "Error zip filename is empty" ) );
    return false;
  }

  int rc = 0;
  const QByteArray zipFileNamePtr = zipFilename.toUtf8();
  struct zip *z = zip_open( zipFileNamePtr.constData(), overwrite ? ( ZIP_CREATE | ZIP_TRUNCATE ) : ZIP_CREATE, &rc );

  if ( rc == ZIP_ER_OK && z )
  {
    // Entries copied from this archive are read when z is closed, so it stays open until then.
    struct zip *reuseArchive = nullptr;
    if ( !reuseFrom.isEmpty() && QFileInfo( reuseFrom ).isFile() )
    {
      int reuseRc = 0;
      reuseArchive = zip_open( reuseFrom.toUtf8().constData(), 0, &reuseRc );
      if ( !reuseArchive )
        QgsDebugMsgLevel( u"Cannot reuse entries of '%1' (error code %2)"_s.arg( reuseFrom ).arg( reuseRc ), 2 );
    }
    const auto closeArchives = [z, reuseArchive] {
      zip_close( z );
      if ( reuseArchive )
        zip_discard( reuseArchive );
    };

    for ( const auto &file : files )
    {
      const QFileInfo fileInfo( file );
      if ( !fileInfo.exists() )
      {
        QgsMessageLog::logMessage( QObject::tr( "Error input file does not exist: '%1'" ).arg( file ) );
        closeArchives();
        return false;
      }

      zip_source *src = reusedEntrySource( z, reuseArchive, fileInfo );
      if ( !src )
      {
        const QByteArray fileNamePtr = file.toUtf8();
        src = zip_source_file( z, fileNamePtr.constData(), 0, 0 );
      }
      if ( src )
      {
        const QByteArray fileInfoPtr = fileInfo.fileName().toUtf8();
#if LIBZIP_VERSION_MAJOR < 1
        rc = ( int ) zip_add( z, fileInfoPtr.constData(), src );
#else
        rc = ( int ) zip_file_add( z, fileInfoPtr.constData(), src, 0 );
#endif
        if ( rc == -1 )
        {
          QgsMessageLog::logMessage( QObject::tr( "Error adding file '%1': %2" ).arg( file, zip_strerror( z ) ) );
          zip_source_free( src );
          closeArchives();
          return false;
        }
      }
      else
      {
        QgsMessageLog::logMessage( QObject::tr( "Error creating data source '%1': %2" ).arg( file, zip_strerror( z ) ) );
        closeArchives();
        return false;
      }
    }

    // libzip writes the archive, including the reused entries, only when closing it.
    const bool closed = zip_close( z ) == 0;
    if ( !closed )
    {
      QgsMessageLog::logMessage( QObject::tr( "Error writing zip archive '%1': %2" ).arg( zipFilename, zip_strerror( z ) ) );
      zip_discard( z );
    }
    if ( reuseArchive )
      zip_discard( reuseArchive );
    if ( !closed )
      return false;
  }
  else
  {
    QgsMessageLog::logMessage( QObject::tr( "Error creating zip archive '%1': %2" ).arg( zipFilename, z ? zip_strerror( z ) : zipFilename ) );
    return false;
  }

  return true;
}

bool QgsZipUtils::decodeGzip( const QByteArray &bytesIn, QByteArray &bytesOut )
{
  return decodeGzip( bytesIn.constData(), bytesIn.count(), bytesOut );
}

bool QgsZipUtils::decodeGzip( const char *bytesIn, std::size_t size, QByteArray &bytesOut )
{
  unsigned char *bytesInPtr = reinterpret_cast<unsigned char *>( const_cast<char *>( bytesIn ) );
  uint bytesInLeft = static_cast<uint>( size );

  const uint CHUNK = 16384;
  unsigned char out[CHUNK];
  const int DEC_MAGIC_NUM_FOR_GZIP = 16;

  // allocate inflate state
  z_stream strm;
  strm.zalloc = Z_NULL;
  strm.zfree = Z_NULL;
  strm.opaque = Z_NULL;
  strm.avail_in = 0;
  strm.next_in = Z_NULL;

  int ret = inflateInit2( &strm, MAX_WBITS + DEC_MAGIC_NUM_FOR_GZIP );
  if ( ret != Z_OK )
  {
    inflateEnd( &strm );
    return false;
  }

  while ( ret != Z_STREAM_END ) // done when inflate() says it's done
  {
    // prepare next chunk
    const uint bytesToProcess = std::min( CHUNK, bytesInLeft );
    strm.next_in = bytesInPtr;
    strm.avail_in = bytesToProcess;
    bytesInPtr += bytesToProcess;
    bytesInLeft -= bytesToProcess;

    if ( bytesToProcess == 0 )
      break; // we end with an error - no more data but inflate() wants more data

    // run inflate() on input until output buffer not full
    do
    {
      strm.avail_out = CHUNK;
      strm.next_out = out;
      ret = inflate( &strm, Z_NO_FLUSH );
      Q_ASSERT( ret != Z_STREAM_ERROR ); // state not clobbered
      if ( ret == Z_NEED_DICT || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR )
      {
        inflateEnd( &strm );
        return false;
      }
      const unsigned have = CHUNK - strm.avail_out;
      bytesOut.append( QByteArray::fromRawData( reinterpret_cast<const char *>( out ), static_cast<int>( have ) ) );
    } while ( strm.avail_out == 0 );
  }

  inflateEnd( &strm );
  return ret == Z_STREAM_END || ( ret == Z_OK && bytesInLeft == 0 );
}

bool QgsZipUtils::encodeGzip( const QByteArray &bytesIn, QByteArray &bytesOut )
{
  unsigned char *bytesInPtr = reinterpret_cast<unsigned char *>( const_cast<char *>( bytesIn.constData() ) );
  const uint bytesInLeft = static_cast<uint>( bytesIn.count() );

  const uint CHUNK = 16384;
  unsigned char out[CHUNK];
  const int DEC_MAGIC_NUM_FOR_GZIP = 16;

  // allocate deflate state
  z_stream strm;
  strm.zalloc = Z_NULL;
  strm.zfree = Z_NULL;
  strm.opaque = Z_NULL;

  int ret = deflateInit2( &strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED, MAX_WBITS + DEC_MAGIC_NUM_FOR_GZIP, 8, Z_DEFAULT_STRATEGY );
  if ( ret != Z_OK )
    return false;

  strm.avail_in = bytesInLeft;
  strm.next_in = bytesInPtr;

  // run deflate() on input until output buffer not full, finish
  // compression if all of source has been read in
  do
  {
    strm.avail_out = CHUNK;
    strm.next_out = out;
    ret = deflate( &strm, Z_FINISH );  // no bad return value
    Q_ASSERT( ret != Z_STREAM_ERROR ); // state not clobbered

    const unsigned have = CHUNK - strm.avail_out;
    bytesOut.append( QByteArray::fromRawData( reinterpret_cast<const char *>( out ), static_cast<int>( have ) ) );
  } while ( strm.avail_out == 0 );
  Q_ASSERT( ret == Z_STREAM_END ); // stream will be complete

  // clean up and return
  deflateEnd( &strm );
  return true;
}

const QStringList QgsZipUtils::files( const QString &zip )
{
  if ( zip.isEmpty() && !QFileInfo::exists( zip ) )
  {
    return QStringList();
  }
  QStringList files;

  int rc = 0;
  const QByteArray fileNamePtr = zip.toUtf8();
  struct zip *z = zip_open( fileNamePtr.constData(), 0, &rc );

  if ( rc == ZIP_ER_OK && z )
  {
    const int count = zip_get_num_entries( z, ZIP_FL_UNCHANGED );
    if ( count != -1 )
    {
      struct zip_stat stat;

      for ( int i = 0; i < count; i++ )
      {
        zip_stat_index( z, i, 0, &stat );
        files << QString( stat.name );
      }
    }

    zip_close( z );
  }

  return files;
}

bool QgsZipUtils::extractFileFromZip( const QString &zipFilename, const QString &filenameInZip, QByteArray &bytesOut )
{
  if ( !QFileInfo::exists( zipFilename ) )
  {
    QgsMessageLog::logMessage( QObject::tr( "Error zip file does not exist: '%1'" ).arg( zipFilename ) );
    return false;
  }

  if ( filenameInZip.isEmpty() )
  {
    QgsMessageLog::logMessage( QObject::tr( "Error file name in zip is empty" ) );
    return false;
  }

  int err = 0;
  const QByteArray zipFilenamePtr = zipFilename.toUtf8();
  struct zip *z = zip_open( zipFilenamePtr.constData(), 0, &err );
  if ( !z )
  {
    zip_error_t error;
    zip_error_init_with_code( &error, err );
    QgsMessageLog::logMessage( QObject::tr( "Error opening zip archive '%1': %2" ).arg( zipFilename, zip_error_strerror( &error ) ) );
    zip_error_fini( &error );
    return false;
  }

  const QByteArray filenameInZipPtr = filenameInZip.toUtf8();
  struct zip_stat st;
  zip_stat_init( &st );
  if ( zip_stat( z, filenameInZipPtr.constData(), 0, &st ) != 0 )
  {
    QgsMessageLog::logMessage( QObject::tr( "File '%1' not found in zip archive '%2': %3" ).arg( filenameInZip, zipFilename, zip_strerror( z ) ) );
    zip_close( z );
    return false;
  }

  zip_file *zf = zip_fopen( z, filenameInZipPtr.constData(), 0 );
  if ( !zf )
  {
    QgsMessageLog::logMessage( QObject::tr( "Could not open file '%1' in zip archive '%2': %3" ).arg( filenameInZip, zipFilename, zip_strerror( z ) ) );
    zip_close( z );
    return false;
  }

  bytesOut.resize( static_cast<int>( st.size ) );
  zip_int64_t readBytes = zip_fread( zf, bytesOut.data(), st.size );

  zip_fclose( zf );
  zip_close( z );

  // If successful, the number of bytes actually read is returned.
  if ( static_cast<zip_uint64_t>( readBytes ) != st.size )
  {
    QgsMessageLog::logMessage( QObject::tr( "Error reading file '%1' from zip archive '%2'." ).arg( filenameInZip, zipFilename ) );
    return false;
  }

  return true;
}
