//=============================================================================
// Copyright (c) 2002 Radical Games Ltd.  All rights reserved.
//=============================================================================


//=============================================================================
//
// File:        win32drive.cpp
//
// Subsystem:   Radical Drive System
//
// Description:	This file contains the implementation of the radWin32Drive class.
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
#include "win32drive.hpp"
#include <filesystem>
#include <fstream>

//=============================================================================
// Public Functions 
//=============================================================================

//=============================================================================
// Function:    radWin32DriveFactory
//=============================================================================
// Description: This member is responsible for constructing a radWin32DriveObject.
//
// Parameters:  pointer to receive drive object
//              pointer to the drive name
//              allocator
//              
// Returns:     
//------------------------------------------------------------------------------

void radWin32DriveFactory
( 
    radDrive**         ppDrive, 
    const char*        pDriveName,
    radMemoryAllocator alloc
)
{
    //
    // Simply constuct the drive object.
    //
    *ppDrive = new( alloc ) radWin32Drive( pDriveName, alloc );
    rAssert( *ppDrive != NULL );
}


//=============================================================================
// Public Member Functions
//=============================================================================

//=============================================================================
// Function:    radWin32Drive::radWin32Drive
//=============================================================================

radWin32Drive::radWin32Drive( const char* pdrivespec, radMemoryAllocator alloc )
    : 
    radDrive( ),
    m_OpenFiles( 0 ),
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
// Function:    radWin32Drive::~radWin32Drive
//=============================================================================

radWin32Drive::~radWin32Drive( void )
{
    m_pMutex->Release( );
    m_pDriveThread->Release( );
}

//=============================================================================
// Function:    radWin32Drive::Lock
//=============================================================================
// Description: Start a critical section
//
// Parameters:  
//
// Returns:     
//------------------------------------------------------------------------------

void radWin32Drive::Lock( void )
{
    m_pMutex->Lock( );
}

//=============================================================================
// Function:    radWin32Drive::Unlock
//=============================================================================
// Description: End a critical section
//
// Parameters:  
//
// Returns:     
//------------------------------------------------------------------------------

void radWin32Drive::Unlock( void )
{
    m_pMutex->Unlock( );
}

//=============================================================================
// Function:    radWin32Drive::GetCapabilities
//=============================================================================

unsigned int radWin32Drive::GetCapabilities( void )
{
    return m_Capabilities;
}

//=============================================================================
// Function:    radGcnDVDDrive::GetDriveName
//=============================================================================

const char* radWin32Drive::GetDriveName( void )
{
    return m_DriveName;
}

//=============================================================================
// Function:    radWin32Drive::Initialize
//=============================================================================

radDrive::CompletionStatus radWin32Drive::Initialize( void )
{
    SetMediaInfo();

    //
    // Success
    //
    m_LastError = Success;
    return Complete;
}

//=============================================================================
// Function:    radWin32Drive::OpenFile
//=============================================================================

radDrive::CompletionStatus radWin32Drive::OpenFile
( 
    const char*         fileName, 
    radFileOpenFlags    flags, 
    bool                writeAccess, 
    radFileHandle*      pHandle, 
    unsigned int*       pSize 
)
{
    char fixedFileName[PATH_MAX];
    size_t fileNameLength = strlen(fileName);

    memcpy(fixedFileName, fileName, fileNameLength + 1);
    for (size_t i = 0; i < fileNameLength; i++) {
        if (fixedFileName[i] == '\\') {
            fixedFileName[i] = '/';
        }
    }

    // FIXME: Actually make use of the flags :)
    switch (flags) {
        case OpenExisting:
            rReleasePrintf("Opening file %s (writeAccess: %d, flags: OpenExisting)\n", fixedFileName, writeAccess);
            break;
        case OpenAlways:
            rReleasePrintf("Opening file %s (writeAccess: %d, flags: OpenAlways)\n", fixedFileName, writeAccess);
            break;
        case CreateAlways:
            rReleasePrintf("Opening file %s (writeAccess: %d, flags: CreateAlways)\n", fixedFileName, writeAccess);
            break;
    }

    *pHandle = SDL_RWFromFile(fixedFileName, writeAccess ? "rb+" : "rb");
    if (*pHandle == NULL)
    {
        rReleasePrintf("Failed to open file %s\n", fixedFileName);
        m_LastError = FileNotFound;
        return Error;
    }

    *pSize = SDL_RWsize(*pHandle);

    m_OpenFiles++;
    m_LastError = Success;
    return Complete;
}

//=============================================================================
// Function:    radWin32Drive::CloseFile
//=============================================================================

radDrive::CompletionStatus radWin32Drive::CloseFile( radFileHandle handle, const char* fileName )
{
    SDL_RWclose(handle);
    m_OpenFiles--;
    return Complete;
}

//=============================================================================
// Function:    radWin32Drive::ReadFile
//=============================================================================

radDrive::CompletionStatus radWin32Drive::ReadFile
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
                "radFileSystem: radWin32Drive: External memory not supported for reads." );

    //
    // set file pointer
    //
    if (SDL_RWseek(handle, position, RW_SEEK_SET) != -1) {
        if (SDL_RWread(handle, pData, 1, bytesToRead) == bytesToRead) {
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
// Function:    radWin32Drive::WriteFile
//=============================================================================

radDrive::CompletionStatus radWin32Drive::WriteFile
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
                "radFileSystem: radWin32Drive: External memory not supported for reads." );

    //
    // do the write
    //
    if (SDL_RWseek(handle, position, RW_SEEK_SET) != -1) {
        if (SDL_RWwrite(handle, pData, 1, bytesToWrite) == bytesToWrite) {
            *bytesWritten = bytesToWrite;
            *pSize = SDL_RWsize(handle);
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
// Function:    radWin32Drive::FindFirst
//=============================================================================

radDrive::CompletionStatus radWin32Drive::FindFirst
( 
    const char*                 searchSpec, 
    IRadDrive::DirectoryInfo*   pDirectoryInfo, 
    radFileDirHandle*           pHandle,
    bool                        firstSearch
)
{
    rAssertMsg(false, "radWin32Drive::FindFirst not implemented");
    //
    // Find first
    //
    std::error_code error;
    *pHandle = std::filesystem::directory_iterator(searchSpec, error);

    //
    // Fill in our directory info structure
    //
    TranslateDirInfo( pDirectoryInfo, **pHandle, pHandle );

    if (!error)
    {
        m_LastError = Success;
        return Complete;
    }
    else
    {
        m_LastError = TranslateError(error);
        return Error;
    }
}

//=============================================================================
// Function:    radWin32Drive::FindNext
//=============================================================================

radDrive::CompletionStatus radWin32Drive::FindNext( radFileDirHandle* pHandle, IRadDrive::DirectoryInfo* pDirectoryInfo )
{
    rAssertMsg(false, "radWin32Drive::FindNext not implemented");
    //
    // If we don't have a handle, return file not found.
    //
    if (*pHandle == std::filesystem::directory_iterator())
    {
        m_LastError = FileNotFound;
        return Error;
    }

    //
    // Find the next entry
    //
    std::error_code error;
    pHandle->increment(error);
    TranslateDirInfo( pDirectoryInfo, **pHandle, pHandle);

    if (!error)
    {
        m_LastError = Success;
        return Complete;
    }
    else
    {
        m_LastError = TranslateError(error);
        return Error;
    }
}

//=============================================================================
// Function:    radWin32Drive::FindClose
//=============================================================================

radDrive::CompletionStatus radWin32Drive::FindClose( radFileDirHandle* pHandle )
{
    rAssertMsg(false, "radWin32Drive::FindClose not implemented");
    *pHandle = std::filesystem::directory_iterator();

    return Complete;
}

//=============================================================================
// Function:    radWin32Drive::CreateDir
//=============================================================================

radDrive::CompletionStatus radWin32Drive::CreateDir( const char* pName )
{
    rAssertMsg(false, "radWin32Drive::CreateDir not implemented");
    rWarningMsg( m_Capabilities & radDriveDirectory, 
        "This drive does not support the CreateDir function." );

    std::error_code error;
    if (std::filesystem::create_directory(pName, error))
    {
        m_LastError = Success;
        return Complete;
    }
    else
    {
        m_LastError = TranslateError(error);
        return Error;
    }
}

//=============================================================================
// Function:    radWin32Drive::DestroyDir
//=============================================================================

radDrive::CompletionStatus radWin32Drive::DestroyDir( const char* pName )
{
    rAssertMsg(false, "radWin32Drive::DestroyDir not implemented");

    rWarningMsg( m_Capabilities & radDriveDirectory,
        "This drive does not support the DestroyDir function." );

    std::error_code error;
    std::filesystem::path path(pName);
    if (std::filesystem::is_directory(path, error) &&
        std::filesystem::remove(path, error))
    {
        m_LastError = Success;
        return Complete;
    }
    else
    {
        m_LastError = TranslateError(error);
        return Error;
    }
}

//=============================================================================
// Function:    radWin32Drive::DestroyFile
//=============================================================================

radDrive::CompletionStatus radWin32Drive::DestroyFile( const char* filename )
{
    rAssertMsg(false, "radWin32Drive::DestroyFile not implemented");

    rWarningMsg( m_Capabilities & radDriveWriteable, "This drive does not support the DestroyFile function." );

    //
    // Someday check if the file is open!
    //

    std::error_code error;
    std::filesystem::path path(filename);
    if (!std::filesystem::is_directory(path, error) &&
        std::filesystem::remove(path, error))
    {
        m_LastError = Success;
        return Complete;
    }
    else
    {
        m_LastError = TranslateError(error);
        return Error;
    }
}

//=============================================================================
// Private Member Functions
//=============================================================================

//=============================================================================
// Function:    radWin32Drive::SetMediaInfo
//=============================================================================

void radWin32Drive::SetMediaInfo( void )
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
        m_MediaInfo.m_FreeSpace = WIN32_DEFAULT_SECTOR_SIZE;
        m_MediaInfo.m_FreeFiles = WIN32_DEFAULT_SECTOR_SIZE;
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

    m_MediaInfo.m_SectorSize = WIN32_DEFAULT_SECTOR_SIZE;
}

//=============================================================================
// Function:    radWin32Drive::TranslateError
//=============================================================================
// Description: This translates the windows error code into our error enums.
//
// Parameters:  Win32 error code
//              
// Returns:     Our error code
//------------------------------------------------------------------------------

radFileError radWin32Drive::TranslateError( std::error_code error )
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

//=============================================================================
// Function:    radWin32Drive::TranslateDirInfo
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

void radWin32Drive::TranslateDirInfo
( 
    IRadDrive::DirectoryInfo*   pDirectoryInfo, 
    const std::filesystem::path directoryEntry,
    const radFileDirHandle*     pHandle
)
{
    if (*pHandle == std::filesystem::directory_iterator())
    {
        //
        // Either we failed or we're out of games.
        //
        pDirectoryInfo->m_Name[0] = '\0';
        pDirectoryInfo->m_Type = IRadDrive::DirectoryInfo::IsDone;
    }
    else
    {
        std::string filename = directoryEntry.filename().u8string();
        strncpy( pDirectoryInfo->m_Name, filename.c_str(), radFileFilenameMax);
        pDirectoryInfo->m_Name[ radFileFilenameMax ] = '\0';
        pDirectoryInfo->m_Type = (*pHandle)->is_directory() ? IRadDrive::DirectoryInfo::IsDirectory : IRadDrive::DirectoryInfo::IsFile;
    }
}
