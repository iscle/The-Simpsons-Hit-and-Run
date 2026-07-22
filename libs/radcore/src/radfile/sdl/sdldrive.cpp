//=============================================================================
// Copyright (c) 2002 Radical Games Ltd.  All rights reserved.
//=============================================================================


//=============================================================================
//
// File:        sdldrive.cpp
//
// Subsystem:   Radical Drive System
//
// Description:	This file contains the implementation of the radSdlDrive class.
//
// Revisions:
//
// Notes:       We keep a serial number when the first file is opened. Then if the
//              media is removed, we don't allow ops until the original serial number
//              is detected, or all files are closed.
//=============================================================================

//=============================================================================
// Include Files
//=============================================================================

#include "pch.hpp"
#include <algorithm>
#include <limits.h>
#include "sdldrive.hpp"
#include <string>
#include <SDL.h>
#if SDL_MAJOR_VERSION < 3
#ifdef WIN32
#include <direct.h>
#else
#include <unistd.h>
#endif
#endif

#ifdef __EMSCRIPTEN__
//=============================================================================
// HTTP-streamed asset files (browser build)
//=============================================================================
//
// Game assets are not preloaded into memory; read-only files that don't
// exist in the local (in-memory) filesystem are opened as SDL_RWops backed
// by HTTP Range requests against /assets/ on the host. Only the chunks a
// read touches are fetched and a small per-file LRU keeps recent ones, so
// resident memory stays proportional to what the game is actually using.
// All calls happen on the drive thread (a pthread), where synchronous
// emscripten_fetch is allowed.
//

#include <emscripten.h>
#include <emscripten/fetch.h>
#include <ctype.h>
#include <map>
#include <list>
#include <vector>

// Monotonic count of files the game has opened. Used by the page's boot
// progress bar (read from the main thread each frame). Written on the drive
// thread; exact synchronization is unnecessary for a progress display.
volatile int g_radFileOpenCount = 0;

namespace
{
    // Save files (Save1..Save4 at the FS root) are the only writes the game
    // makes. They live in the in-memory filesystem, so after each save is
    // written we mirror it to IndexedDB (see Module.persistSave in the shell)
    // and restore it before boot, giving persistent saves across reloads.
    bool radEmIsSaveFile( const char* fileName )
    {
        const char* p = strstr( fileName, "Save" );
        return p != NULL && isdigit( (unsigned char) p[ 4 ] );
    }

    void radEmPersistSave( const char* fileName )
    {
        // Normalize to the root-relative path the JS FS uses ("/SaveN").
        const char* base = fileName;
        for ( const char* p = fileName; *p != '\0'; p++ )
        {
            if ( *p == '/' || *p == '\\' )
            {
                base = p + 1;
            }
        }
        char path[ 64 ];
        snprintf( path, sizeof( path ), "/%s", base );
        MAIN_THREAD_EM_ASM(
            { if ( Module.persistSave ) Module.persistSave( UTF8ToString( $0 ) ); },
            path );
    }

    const unsigned int kHttpChunkBytes = 1024 * 1024;
    const unsigned int kHttpMaxCachedChunks = 8; // per open file

    struct radEmHttpFile
    {
        char m_Url[ 560 ];
        Sint64 m_Size;
        Sint64 m_Pos;
        std::map< unsigned int, std::vector< unsigned char > > m_Chunks;
        std::list< unsigned int > m_Lru;
    };

    Sint64 radEmHttpGetSize( const char* url )
    {
        emscripten_fetch_attr_t attr;
        emscripten_fetch_attr_init( &attr );
        strcpy( attr.requestMethod, "HEAD" );
        attr.attributes = EMSCRIPTEN_FETCH_SYNCHRONOUS;

        emscripten_fetch_t* fetch = emscripten_fetch( &attr, url );
        if ( fetch == NULL )
        {
            return -1;
        }

        Sint64 size = -1;
        if ( fetch->status == 200 )
        {
            size_t headersLength = emscripten_fetch_get_response_headers_length( fetch );
            std::vector< char > headers( headersLength + 1 );
            emscripten_fetch_get_response_headers( fetch, &headers[ 0 ], headersLength + 1 );
            for ( char* p = &headers[ 0 ]; *p != '\0'; p++ )
            {
                *p = ( char ) tolower( *p );
            }
            const char* contentLength = strstr( &headers[ 0 ], "content-length:" );
            if ( contentLength != NULL )
            {
                size = strtoll( contentLength + 15, NULL, 10 );
            }
        }
        emscripten_fetch_close( fetch );
        return size;
    }

    bool radEmHttpFetchChunk( radEmHttpFile* pFile, unsigned int chunkIndex )
    {
        Sint64 start = ( Sint64 ) chunkIndex * kHttpChunkBytes;
        Sint64 end = start + kHttpChunkBytes - 1;
        if ( end >= pFile->m_Size )
        {
            end = pFile->m_Size - 1;
        }

        char range[ 64 ];
        snprintf( range, sizeof( range ), "bytes=%lld-%lld",
            ( long long ) start, ( long long ) end );
        const char* headers[] = { "Range", range, NULL };

        emscripten_fetch_attr_t attr;
        emscripten_fetch_attr_init( &attr );
        strcpy( attr.requestMethod, "GET" );
        attr.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY | EMSCRIPTEN_FETCH_SYNCHRONOUS;
        attr.requestHeaders = headers;

        emscripten_fetch_t* fetch = emscripten_fetch( &attr, pFile->m_Url );
        if ( fetch == NULL )
        {
            return false;
        }

        bool ok = ( fetch->status == 206 || fetch->status == 200 ) && fetch->numBytes > 0;
        if ( ok )
        {
            std::vector< unsigned char > & chunk = pFile->m_Chunks[ chunkIndex ];
            chunk.assign( ( const unsigned char* ) fetch->data,
                ( const unsigned char* ) fetch->data + fetch->numBytes );
            pFile->m_Lru.push_back( chunkIndex );

            while ( pFile->m_Lru.size( ) > kHttpMaxCachedChunks )
            {
                pFile->m_Chunks.erase( pFile->m_Lru.front( ) );
                pFile->m_Lru.pop_front( );
            }
        }
        emscripten_fetch_close( fetch );
        return ok;
    }

    Sint64 radEmHttpRwSize( SDL_RWops* pContext )
    {
        return ( ( radEmHttpFile* ) pContext->hidden.unknown.data1 )->m_Size;
    }

    Sint64 radEmHttpRwSeek( SDL_RWops* pContext, Sint64 offset, int whence )
    {
        radEmHttpFile* pFile = ( radEmHttpFile* ) pContext->hidden.unknown.data1;
        Sint64 newPos = pFile->m_Pos;
        switch ( whence )
        {
            case RW_SEEK_SET: newPos = offset; break;
            case RW_SEEK_CUR: newPos = pFile->m_Pos + offset; break;
            case RW_SEEK_END: newPos = pFile->m_Size + offset; break;
        }
        if ( newPos < 0 )
        {
            return -1;
        }
        pFile->m_Pos = newPos;
        return newPos;
    }

    size_t radEmHttpRwRead( SDL_RWops* pContext, void* ptr, size_t size, size_t maxnum )
    {
        radEmHttpFile* pFile = ( radEmHttpFile* ) pContext->hidden.unknown.data1;

        Sint64 totalBytes = ( Sint64 )( size * maxnum );
        if ( pFile->m_Pos >= pFile->m_Size )
        {
            return 0;
        }
        if ( pFile->m_Pos + totalBytes > pFile->m_Size )
        {
            totalBytes = pFile->m_Size - pFile->m_Pos;
        }

        Sint64 bytesRead = 0;
        unsigned char* pOut = ( unsigned char* ) ptr;

        while ( bytesRead < totalBytes )
        {
            Sint64 pos = pFile->m_Pos + bytesRead;
            unsigned int chunkIndex = ( unsigned int )( pos / kHttpChunkBytes );

            std::map< unsigned int, std::vector< unsigned char > >::iterator it =
                pFile->m_Chunks.find( chunkIndex );
            if ( it == pFile->m_Chunks.end( ) )
            {
                if ( !radEmHttpFetchChunk( pFile, chunkIndex ) )
                {
                    break;
                }
                it = pFile->m_Chunks.find( chunkIndex );
            }

            Sint64 chunkOffset = pos - ( Sint64 ) chunkIndex * kHttpChunkBytes;
            Sint64 available = ( Sint64 ) it->second.size( ) - chunkOffset;
            if ( available <= 0 )
            {
                break;
            }
            Sint64 copyBytes = totalBytes - bytesRead;
            if ( copyBytes > available )
            {
                copyBytes = available;
            }
            memcpy( pOut + bytesRead, &it->second[ chunkOffset ], ( size_t ) copyBytes );
            bytesRead += copyBytes;
        }

        pFile->m_Pos += bytesRead;
        return ( size_t )( size > 0 ? bytesRead / ( Sint64 ) size : 0 );
    }

    size_t radEmHttpRwWrite( SDL_RWops* pContext, const void* ptr, size_t size, size_t num )
    {
        (void) pContext; (void) ptr; (void) size; (void) num;
        return 0;
    }

    int radEmHttpRwClose( SDL_RWops* pContext )
    {
        delete ( radEmHttpFile* ) pContext->hidden.unknown.data1;
        SDL_FreeRW( pContext );
        return 0;
    }

    // Base URL that game asset requests are resolved against. Defaults to the
    // same-origin "/assets/", but the page can override it (Module.assetBaseUrl)
    // so the build can be hosted somewhere that can't serve the ~1.8GB of data
    // itself (e.g. GitHub Pages) and stream assets from a separate host. Read
    // once, lazily, from the main thread. The value must end with '/'.
    const char* radEmAssetBase( void )
    {
        static char base[ 512 ] = { 0 };
        static bool initialized = false;
        if ( !initialized )
        {
            initialized = true;
            MAIN_THREAD_EM_ASM( {
                var s = ( Module.assetBaseUrl || '/assets/' );
                if ( s.charAt( s.length - 1 ) !== '/' ) { s += '/'; }
                stringToUTF8( s, $0, 512 );
            }, base );
            if ( base[ 0 ] == '\0' )
            {
                strcpy( base, "/assets/" );
            }
        }
        return base;
    }

    SDL_RWops* radEmHttpOpen( const char* fullName )
    {
        const char* relative = fullName;
        while ( *relative == '/' )
        {
            relative++;
        }
        if ( *relative == '\0' )
        {
            return NULL;
        }

        radEmHttpFile* pFile = new radEmHttpFile;
        snprintf( pFile->m_Url, sizeof( pFile->m_Url ), "%s%s", radEmAssetBase(), relative );
        pFile->m_Pos = 0;
        pFile->m_Size = radEmHttpGetSize( pFile->m_Url );
        if ( pFile->m_Size < 0 )
        {
            delete pFile;
            return NULL;
        }

        SDL_RWops* pRw = SDL_AllocRW( );
        if ( pRw == NULL )
        {
            delete pFile;
            return NULL;
        }
        pRw->size = radEmHttpRwSize;
        pRw->seek = radEmHttpRwSeek;
        pRw->read = radEmHttpRwRead;
        pRw->write = radEmHttpRwWrite;
        pRw->close = radEmHttpRwClose;
        pRw->type = SDL_RWOPS_UNKNOWN;
        pRw->hidden.unknown.data1 = pFile;
        return pRw;
    }
}
#endif // __EMSCRIPTEN__

//=============================================================================
// Public Functions
//=============================================================================

//=============================================================================
// Function:    radSdlDriveFactory
//=============================================================================
// Description: This member is responsible for constructing a radSdlDriveObject.
//
// Parameters:  pointer to receive drive object
//              pointer to the drive name
//              allocator
//              
// Returns:     
//------------------------------------------------------------------------------

void radSdlDriveFactory
( 
    radDrive**         ppDrive, 
    const char*        pDriveName,
    radMemoryAllocator alloc
)
{
    //
    // Simply constuct the drive object.
    //
    *ppDrive = new( alloc ) radSdlDrive( pDriveName, alloc );
    rAssert( *ppDrive != NULL );
}


//=============================================================================
// Public Member Functions
//=============================================================================

//=============================================================================
// Function:    radSdlDrive::radSdlDrive
//=============================================================================

radSdlDrive::radSdlDrive( const char* pdrivespec, radMemoryAllocator alloc )
    :
    radDrive( ),
    m_OpenFiles( 0 ),
    m_pMutex( NULL )
{
    //
    // The default-drive path is set from getcwd() below, gated on
    // m_DrivePath being empty. That relies on zero-initialized memory, which
    // the heap allocator does not guarantee (e.g. under Emscripten WasmFS the
    // buffer is garbage and getcwd() gets skipped, corrupting every path).
    //
    m_DriveName[ 0 ] = '\0';
    m_DrivePath[ 0 ] = '\0';

    //
    // Create a mutex for lock/unlock
    //
    radThreadCreateMutex( &m_pMutex, alloc );
    rAssert( m_pMutex != NULL );

    //
    // Create the drive thread.
    //
    m_pDriveThread = new( alloc ) radDriveThread( m_pMutex, alloc );
    rAssert( m_pDriveThread != NULL );

    //
    // Copy the drivename
    //
    radGetDefaultDrive( m_DriveName );
    if ( strcmp(m_DriveName, pdrivespec ) != 0 )
    {
        strncpy( m_DriveName, pdrivespec, radFileDrivenameMax );
        strncpy( m_DrivePath, pdrivespec, radFileFilenameMax );
        m_DriveName[radFileDrivenameMax] = '\0';
        m_DrivePath[radFileFilenameMax] = '\0';
        SDL_strupr( m_DriveName );
        SDL_strlwr( m_DrivePath );
    }

    if(!m_DrivePath[0])
    {
#if SDL_MAJOR_VERSION < 3
#ifdef WIN32
        _getcwd( m_DrivePath, radFileFilenameMax );
        strncat(m_DrivePath, "/", radFileFilenameMax);
#else
        getcwd( m_DrivePath, radFileFilenameMax );
        strncat(m_DrivePath, "/", radFileFilenameMax);
#endif
#else
        char* cwd = SDL_GetCurrentDirectory();
        strncpy(m_DrivePath, cwd, radFileFilenameMax);
        SDL_free(cwd);
#endif
        m_DrivePath[radFileFilenameMax] = '\0';
    }

#if SDL_MAJOR_VERSION < 3
    m_Capabilities = ( radDriveWriteable | radDriveFile );
#else
    m_Capabilities = ( radDriveEnumerable | radDriveWriteable | radDriveDirectory | radDriveFile );
#endif
}

//=============================================================================
// Function:    radSdlDrive::~radSdlDrive
//=============================================================================

radSdlDrive::~radSdlDrive( void )
{
    m_pMutex->Release( );
    m_pDriveThread->Release( );
}

//=============================================================================
// Function:    radSdlDrive::Lock
//=============================================================================
// Description: Start a critical section
//
// Parameters:  
//
// Returns:     
//------------------------------------------------------------------------------

void radSdlDrive::Lock( void )
{
    m_pMutex->Lock( );
}

//=============================================================================
// Function:    radSdlDrive::Unlock
//=============================================================================
// Description: End a critical section
//
// Parameters:  
//
// Returns:     
//------------------------------------------------------------------------------

void radSdlDrive::Unlock( void )
{
    m_pMutex->Unlock( );
}

//=============================================================================
// Function:    radSdlDrive::GetCapabilities
//=============================================================================

unsigned int radSdlDrive::GetCapabilities( void )
{
    return m_Capabilities;
}

//=============================================================================
// Function:    radGcnDVDDrive::GetDriveName
//=============================================================================

const char* radSdlDrive::GetDriveName( void )
{
    return m_DriveName;
}

//=============================================================================
// Function:    radSdlDrive::Initialize
//=============================================================================

radDrive::CompletionStatus radSdlDrive::Initialize( void )
{
    SetMediaInfo();

    //
    // Success
    //
    m_LastError = Success;
    return Complete;
}

//=============================================================================
// Function:    radSdlDrive::OpenFile
//=============================================================================

radDrive::CompletionStatus radSdlDrive::OpenFile
( 
    const char*         fileName, 
    radFileOpenFlags    flags, 
    bool                writeAccess, 
    radFileHandle*      pHandle, 
    unsigned int*       pSize 
)
{
    //
    // Build the full filename
    //
    char fullName[ radFileFilenameMax + 1 ];
    BuildFileSpec( fileName, fullName, radFileFilenameMax + 1 );

    //
    // Translate flags to SDL
    //
    const char* createFlags;
    switch( flags )
    {
    case OpenExisting:
        createFlags = writeAccess ? "rb+" : "rb";
        break;
    case OpenAlways:
        createFlags = "ab+";
        break;
    case CreateAlways:
        createFlags = "wb+";
        break;
    default:
        rAssertMsg( false, "radFileSystem: sdldrive: attempting to open file with unknown flag" );
        return Error;
    }

#if SDL_MAJOR_VERSION < 3
    *pHandle = SDL_RWFromFile(fullName, createFlags);
#else
    *pHandle = SDL_IOFromFile( fullName, createFlags );
#endif

#ifdef __EMSCRIPTEN__
    //
    // Read-only files missing from the local in-memory filesystem (i.e. the
    // game assets) are streamed from the server on demand.
    //
    if ( *pHandle == NULL && flags == OpenExisting && !writeAccess )
    {
        *pHandle = radEmHttpOpen( fullName );
    }
#endif

    if ( *pHandle )
    {
        m_OpenFiles++;
#ifdef __EMSCRIPTEN__
        g_radFileOpenCount++;
#endif
#if SDL_MAJOR_VERSION < 3
        *pSize = SDL_RWsize( (SDL_RWops*)*pHandle );
#else
        *pSize = SDL_GetIOSize( (SDL_IOStream*)*pHandle );
#endif
        m_LastError = Success;
        return Complete;
    }
    else
    {
#if defined(__EMSCRIPTEN__) && defined(SRR2_DEBUG_FILEIO)
        SDL_Log( "radSdlDrive::OpenFile FileNotFound: fullName='%s' (in='%s' drivePath='%s')",
                 fullName, fileName, m_DrivePath );
#endif
        m_LastError = FileNotFound;
        return Error;
    }
}

//=============================================================================
// Function:    radSdlDrive::CloseFile
//=============================================================================

radDrive::CompletionStatus radSdlDrive::CloseFile( radFileHandle handle, const char* fileName )
{
#if SDL_MAJOR_VERSION < 3
    SDL_RWclose( (SDL_RWops*)handle );
#else
    SDL_CloseIO( (SDL_IOStream*)handle );
#endif
    m_OpenFiles--;

#ifdef __EMSCRIPTEN__
    //
    // A save file was just flushed and closed; mirror it to persistent
    // storage. (Harmless if it was only opened for reading -- the bytes are
    // the same.)
    //
    if ( fileName != NULL && radEmIsSaveFile( fileName ) )
    {
        radEmPersistSave( fileName );
    }
#endif

    return Complete;
}

//=============================================================================
// Function:    radSdlDrive::ReadFile
//=============================================================================

radDrive::CompletionStatus radSdlDrive::ReadFile
( 
    radFileHandle   handle, 
    const char*     fileName,
    IRadFile::BufferedReadState buffState,
    unsigned int    position, 
    void*           pData, 
    unsigned int    bytesToRead, 
    unsigned int*   bytesRead, 
    radMemorySpace  pDataSpace 
)
{
    rAssertMsg( pDataSpace == radMemorySpace_Local, 
                "radFileSystem: radSdlDrive: External memory not supported for reads." );

    //
    // set file pointer
    //
#if SDL_MAJOR_VERSION < 3
    if ( SDL_RWseek( (SDL_RWops*)handle, position, RW_SEEK_SET ) >= 0 )
    {
        if (SDL_RWread( (SDL_RWops*)handle, pData, 1, bytesToRead ) > 0 )
        {
#else
    if ( SDL_SeekIO( (SDL_IOStream*)handle, position, SDL_IO_SEEK_SET ) >= 0 )
    {
        if ( SDL_ReadIO( (SDL_IOStream*)handle, pData, bytesToRead ) > 0 )
        {
#endif
            //
            // Successful read!
            //
            
            //
            // Change this during buffered read!!
            //
            *bytesRead = bytesToRead;
            m_LastError = Success;
            return Complete;
        }
    }

    //
    // Failed!
    //
    m_LastError = FileNotFound;
    return Error;
}

//=============================================================================
// Function:    radSdlDrive::WriteFile
//=============================================================================

radDrive::CompletionStatus radSdlDrive::WriteFile
( 
    radFileHandle     handle,
    const char*       fileName,
    IRadFile::BufferedReadState buffState,
    unsigned int      position, 
    const void*       pData, 
    unsigned int      bytesToWrite, 
    unsigned int*     bytesWritten, 
    unsigned int*     pSize, 
    radMemorySpace    pDataSpace 
)
{
    if ( !( m_Capabilities & radDriveWriteable ) )
    {
        rWarningMsg( m_Capabilities & radDriveWriteable, "This drive does not support the WriteFile function." );
        return Error;
    }

    rAssertMsg( pDataSpace == radMemorySpace_Local, 
                "radFileSystem: radSdlDrive: External memory not supported for reads." );

    //
    // do the write
    //
#if SDL_MAJOR_VERSION < 3
    if ( SDL_RWseek( (SDL_RWops*)handle, position, RW_SEEK_SET ) >= 0 )
    {
        *bytesWritten = SDL_RWwrite( (SDL_RWops*)handle, pData, 1, bytesToWrite );
#else
    if ( SDL_SeekIO( (SDL_IOStream*)handle, position, SDL_IO_SEEK_SET ) >= 0 )
    {
        *bytesWritten = SDL_WriteIO( (SDL_IOStream*)handle, pData, bytesToWrite );
#endif
        if ( *bytesWritten == bytesToWrite )
        {
            //
            // Sucessful write
            //
#if SDL_MAJOR_VERSION < 3
            *pSize = SDL_RWsize( (SDL_RWops*)handle );
#else
            *pSize = SDL_GetIOSize( (SDL_IOStream*)handle );
#endif
            m_LastError = Success;
            return Complete;
        }
    }

    //
    // Failed!
    //
    m_LastError = FileNotFound;
    return Error;
}

#if SDL_MAJOR_VERSION > 2
//=============================================================================
// Function:    radSdlDrive::FindFirst
//=============================================================================

radDrive::CompletionStatus radSdlDrive::FindFirst
( 
    const char*                 searchSpec, 
    IRadDrive::DirectoryInfo*   pDirectoryInfo, 
    radFileDirHandle*           pHandle,
    bool                        firstSearch
)
{
    //
    // Find first
    //
    const char* pattern = strrchr(searchSpec, '\\');
    std::string path;
    if (!pattern)
        pattern = strrchr(searchSpec, '/');
    if (pattern)
        path = std::string(searchSpec, pattern - searchSpec);
    else
        pattern = searchSpec;
    path = m_DrivePath + path;
    std::replace(path.begin(), path.end(), '\\', '/');

    char** handle = SDL_GlobDirectory( path.c_str(), pattern, SDL_GLOB_CASEINSENSITIVE, NULL );
    if ( handle )
    {
        SDL_PathInfo info;
        if ( SDL_GetPathInfo( handle[0], &info))
            m_LastError = TranslateDirInfo( pDirectoryInfo, &info, pHandle );
        else
            m_LastError = TranslateDirInfo( pDirectoryInfo, NULL, pHandle );
        // HACK: We don't need the first element anymore, so use it to store the iterator
        ((char***)handle)[0] = handle;
    }
    else
    {
        m_LastError = FileNotFound;
    }

    //
    // Fill in our directory info structure
    //
    if ( m_LastError == Success )
    {
        return Complete;
    }
    else
    {
        return Error;
    }
}

//=============================================================================
// Function:    radSdlDrive::FindNext
//=============================================================================

radDrive::CompletionStatus radSdlDrive::FindNext( radFileDirHandle* pHandle, IRadDrive::DirectoryInfo* pDirectoryInfo )
{
    //
    // If we don't have a handle, return file not found.
    //
    if ( *pHandle == NULL )
    {
        m_LastError = FileNotFound;
        return Error;
    }

    //
    // Find the next entry
    //
    char*** handle = (char***)*pHandle;
    *handle++;
    SDL_PathInfo info;
    if ( SDL_GetPathInfo(  **handle, &info ))
        m_LastError = TranslateDirInfo( pDirectoryInfo, &info, pHandle );
    else
        m_LastError = TranslateDirInfo( pDirectoryInfo, NULL, pHandle );
    
    if ( m_LastError == Success )
    {
        m_LastError = Success;
        return Complete;
    }
    else
    {
        m_LastError = FileNotFound;
        return Error;
    }
}

//=============================================================================
// Function:    radSdlDrive::FindClose
//=============================================================================

radDrive::CompletionStatus radSdlDrive::FindClose( radFileDirHandle* pHandle )
{
    SDL_free( *pHandle );
    *pHandle = NULL;

    return Complete;
}

//=============================================================================
// Function:    radSdlDrive::CreateDir
//=============================================================================

radDrive::CompletionStatus radSdlDrive::CreateDir( const char* pName )
{
    rWarningMsg( m_Capabilities & radDriveDirectory, 
        "This drive does not support the CreateDir function." );

    //
    // Build the full filename
    //
    char fullSpec[ radFileFilenameMax + 1 ];
    BuildFileSpec( pName, fullSpec, radFileFilenameMax + 1 );

    if ( SDL_CreateDirectory( fullSpec ) )
    {
        m_LastError = Success;
        return Complete;
    }
    else
    {
        m_LastError = FileNotFound;
        return Error;
    }
}

//=============================================================================
// Function:    radSdlDrive::DestroyDir
//=============================================================================

radDrive::CompletionStatus radSdlDrive::DestroyDir( const char* pName )
{
    rWarningMsg( m_Capabilities & radDriveDirectory,
        "This drive does not support the DestroyDir function." );

    //
    // Someday check if pName is a dir!
    //

    //
    // Build the full filename
    //
    char fullSpec[ radFileFilenameMax + 1 ];
    BuildFileSpec( pName, fullSpec, radFileFilenameMax + 1 );

    if ( SDL_RemovePath( fullSpec ) )
    {
        m_LastError = Success;
        return Complete;
    }
    else
    {
        m_LastError = FileNotFound;
        return Error;
    }
}

//=============================================================================
// Function:    radSdlDrive::DestroyFile
//=============================================================================

radDrive::CompletionStatus radSdlDrive::DestroyFile( const char* filename )
{
    rWarningMsg( m_Capabilities & radDriveWriteable, "This drive does not support the DestroyFile function." );

    //
    // Someday check if the file is open!
    //

    //
    // Build the full filename
    //
    char fullSpec[ radFileFilenameMax + 1 ];
    BuildFileSpec( filename, fullSpec, radFileFilenameMax + 1 );

    if ( SDL_RemovePath( fullSpec ) )
    {
        m_LastError = Success;
        return Complete;
    }
    else
    {
        m_LastError = FileNotFound;
        return Error;
    }
}
#endif

//=============================================================================
// Private Member Functions
//=============================================================================

//=============================================================================
// Function:    radSdlDrive::SetMediaInfo
//=============================================================================

void radSdlDrive::SetMediaInfo( void )
{
    //
    // Get volume information.
    //
    const char* realDriveName = m_DriveName;

    //rAssert( strlen( realDriveName ) == 2 );
    strcpy(m_MediaInfo.m_VolumeName, realDriveName );
    //strcat(m_MediaInfo.m_VolumeName, "\\");

    m_MediaInfo.m_SectorSize = SDL_DEFAULT_SECTOR_SIZE;

    /*
    if(!error)
    {
        m_MediaInfo.m_MediaState = IRadDrive::MediaInfo::MediaPresent;
        m_MediaInfo.m_FreeSpace = space.free;

        //
        // No file limit, so set it to the available space
        //
        m_MediaInfo.m_FreeFiles = space.available / m_MediaInfo.m_SectorSize;
        m_LastError = Success;
    }
    else
    */
    {
        //
        // Don't have media info, so fill structure in with dummy info
        //
        m_MediaInfo.m_MediaState = IRadDrive::MediaInfo::MediaPresent;
        m_MediaInfo.m_FreeSpace = UINT_MAX;
        m_MediaInfo.m_FreeFiles = m_MediaInfo.m_FreeSpace / m_MediaInfo.m_SectorSize;
        m_LastError = Success;
    }
}

//=============================================================================
// Function:    radSdlDrive::BuildFileSpec
//=============================================================================

void radSdlDrive::BuildFileSpec( const char* fileName, char* fullName, unsigned int size )
{
    std::string path(m_DrivePath);
    path += fileName;
    std::replace(path.begin(), path.end(), '\\', '/');

    strncpy( fullName, path.c_str(), size - 1 );
    fullName[ size - 1 ] = '\0';
}

#if SDL_MAJOR_VERSION > 2
//=============================================================================
// Function:    radSdlDrive::TranslateDirInfo
//=============================================================================
// Description: Translate the directory info and return an error status. A handle
//              with value directory_iterator() means the find_first/next call
//              failed and needs to be checked if something went wrong or if the
//              search just ended.
//
// Parameters:  
//              
// Returns:     
//------------------------------------------------------------------------------

radFileError radSdlDrive::TranslateDirInfo
( 
    IRadDrive::DirectoryInfo*   pDirectoryInfo, 
    const SDL_PathInfo*         pPathInfo,
    const radFileDirHandle*     pHandle
)
{
    char*** handle = (char***)*pHandle;
    if ( !pPathInfo || !*handle )
    {
        //
        // Either we failed or we're out of games.
        //
        if ( !pPathInfo )
        {
            return FileNotFound;
        }
        else
        {
            pDirectoryInfo->m_Name[0] = '\0';
            pDirectoryInfo->m_Type = IRadDrive::DirectoryInfo::IsDone;
        }
    }
    else
    {
        strncpy( pDirectoryInfo->m_Name, **handle, radFileFilenameMax );
        pDirectoryInfo->m_Name[ radFileFilenameMax ] = '\0';

        if ( pPathInfo->type == SDL_PATHTYPE_DIRECTORY )
        {
            pDirectoryInfo->m_Type = IRadDrive::DirectoryInfo::IsDirectory;
        }
        else
        {
            pDirectoryInfo->m_Type = IRadDrive::DirectoryInfo::IsFile;
        }
    }
    return Success;
}
#endif
