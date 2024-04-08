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
#include <filesystem>
#include <fstream>

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
    m_pMutex( NULL )
{
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
    strncpy( m_DriveName, pdrivespec, radFileDrivenameMax );
    m_DriveName[ radFileDrivenameMax ] = '\0';
    m_Capabilities = ( radDriveEnumerable | radDriveWriteable | radDriveDirectory | radDriveFile );
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
    char fixedPath[PATH_MAX];
    FixPath(fixedPath, fileName);

    switch (flags) {
        case OpenExisting:
            *pHandle = SDL_RWFromFile(fixedPath, writeAccess ? "rb+" : "rb");
            break;
        case OpenAlways:
            *pHandle = SDL_RWFromFile(fixedPath, writeAccess ? "ab" : "ab+");
            break;
        case CreateAlways:
            *pHandle = SDL_RWFromFile(fixedPath, writeAccess ? "wb" : "wb+");
            break;
    }
    if (*pHandle == NULL) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to open file %s (%s): %s\n", fixedPath, fileName, SDL_GetError());
        m_LastError = FileNotFound;
        return Error;
    }

    *pSize = SDL_RWsize(*pHandle);

    m_LastError = Success;
    return Complete;
}

//=============================================================================
// Function:    radSdlDrive::CloseFile
//=============================================================================

radDrive::CompletionStatus radSdlDrive::CloseFile( radFileHandle handle, const char* fileName )
{
    SDL_RWclose(handle);
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

    int ret;

    ret = SDL_RWseek(handle, position, RW_SEEK_SET);
    if (ret == -1) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to seek file %s: %s\n", fileName, SDL_GetError());
        m_LastError = FileNotFound;
        return Error;
    }

    ret = SDL_RWread(handle, pData, 1, bytesToRead);
    if (ret == 0) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to read file %s: %s (%d bytes read)\n", fileName, SDL_GetError(), ret);
        m_LastError = FileNotFound;
        return Error;
    }

    *bytesRead = ret;
    m_LastError = Success;
    return Complete;
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
    rAssertMsg( pDataSpace == radMemorySpace_Local,
                "radFileSystem: radSdlDrive: External memory not supported for reads." );

    int ret;

    ret = SDL_RWseek(handle, position, RW_SEEK_SET);
    if (ret == -1) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to seek file %s: %s\n", fileName, SDL_GetError());
        m_LastError = FileNotFound;
        return Error;
    }

    ret = SDL_RWwrite(handle, pData, 1, bytesToWrite);
    if (ret != bytesToWrite) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Failed to write file %s: %s (%d bytes written)\n", fileName, SDL_GetError(), ret);
        m_LastError = FileNotFound;
        return Error;
    }

    *bytesWritten = ret;
    *pSize = SDL_RWsize(handle);

    m_LastError = Success;
    return Complete;
}

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
    // Not implemented
    return Error;
}

//=============================================================================
// Function:    radSdlDrive::FindNext
//=============================================================================

radDrive::CompletionStatus radSdlDrive::FindNext( radFileDirHandle* pHandle, IRadDrive::DirectoryInfo* pDirectoryInfo )
{
    // Not implemented
    return Error;
}

//=============================================================================
// Function:    radSdlDrive::FindClose
//=============================================================================

radDrive::CompletionStatus radSdlDrive::FindClose( radFileDirHandle* pHandle )
{
    // Not implemented
    return Error;
}

//=============================================================================
// Function:    radSdlDrive::CreateDir
//=============================================================================

radDrive::CompletionStatus radSdlDrive::CreateDir( const char* pName )
{
    // Not implemented
    return Error;
}

//=============================================================================
// Function:    radSdlDrive::DestroyDir
//=============================================================================

radDrive::CompletionStatus radSdlDrive::DestroyDir( const char* pName )
{
    // Not implemented
    return Error;
}

//=============================================================================
// Function:    radSdlDrive::DestroyFile
//=============================================================================

radDrive::CompletionStatus radSdlDrive::DestroyFile( const char* filename )
{
    // Not implemented
    return Error;
}

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
    
    std::error_code error;
    std::filesystem::space_info space = std::filesystem::space(realDriveName, error);
    if (!error)
    {
        m_MediaInfo.m_MediaState = IRadDrive::MediaInfo::MediaPresent;
        m_MediaInfo.m_FreeSpace = space.free;

        //
        // No file limit, so set it to the available space
        //
        m_MediaInfo.m_FreeFiles = space.available;
        m_LastError = Success;
    }
    else
    {
#ifdef RAD_VITA
        //
        // Don't have media info, so fill structure in with dummy info
        //
        m_MediaInfo.m_MediaState = IRadDrive::MediaInfo::MediaPresent;
        m_MediaInfo.m_FreeSpace = SDL_DEFAULT_SECTOR_SIZE;
        m_MediaInfo.m_FreeFiles = SDL_DEFAULT_SECTOR_SIZE;
#else
        //
        // Don't have media info, so fill structure in with 0s
        //
        m_MediaInfo.m_MediaState = IRadDrive::MediaInfo::MediaNotPresent;
        m_MediaInfo.m_FreeSpace = 0;
        m_MediaInfo.m_FreeFiles = 0;
#endif
        m_LastError = TranslateError(error);
    }

    m_MediaInfo.m_SectorSize = SDL_DEFAULT_SECTOR_SIZE;
}

//=============================================================================
// Function:    radSdlDrive::TranslateError
//=============================================================================
// Description: This translates the windows error code into our error enums.
//
// Parameters:  Sdl error code
//              
// Returns:     Our error code
//------------------------------------------------------------------------------

radFileError radSdlDrive::TranslateError( std::error_code error )
{
    //
    // we shouldn't have success here. If we do we made a mistake earlier.
    //
    switch( (std::errc)error.default_error_condition().value() )
    {
    case std::errc::no_such_file_or_directory:
    case std::errc::no_such_device:
        return( FileNotFound );

    case std::errc::cross_device_link:
        return( WrongMedia );

    case std::errc::device_or_resource_busy:
    case std::errc::resource_unavailable_try_again:
        return( ShellOpen );

    case std::errc::no_space_on_device:
        return ( NoFreeSpace );

    default:
        return( HardwareFailure );
    }
}

void radSdlDrive::FixPath( char *fixedPath, const char *path )
{
    size_t pathLength = strlen(path);
    memcpy(fixedPath, path, pathLength + 1);
    for (size_t i = 0; i < pathLength; i++) {
        if (fixedPath[i] == '\\') {
            fixedPath[i] = '/';
        }
    }
}
