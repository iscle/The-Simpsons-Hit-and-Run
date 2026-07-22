//=============================================================================
// Copyright (c) 2002 Radical Games Ltd.  All rights reserved.
//=============================================================================

#include "pch.hpp"
#include "system.hpp"
#include "listener.hpp"
#include "buffer.hpp"
#include "voice.hpp"
#include "../common/banner.hpp"
#include "../common/memoryregion.hpp"
#include <radplatform.hpp>
#include <efx.h>
#include "radinprogext.h"

LPALBUFFERSTORAGESOFT radBufferStorageSOFT;
LPALMAPBUFFERSOFT radMapBufferSOFT;
LPALUNMAPBUFFERSOFT radUnmapBufferSOFT;

#ifdef __EMSCRIPTEN__
//
// Emscripten's OpenAL lacks AL_SOFT_map_buffer. Emulate mappable buffers by
// handing out the caller's client-side copy on Map and re-uploading the whole
// buffer with alBufferData on Unmap. Uploads to a buffer attached to a playing
// source are dropped by AL; the error is swallowed below.
//
#include <map>
#include <deque>
#include <pthread.h>

namespace
{
    struct radEmQueuedChunk
    {
        ALuint m_Buffer;
        unsigned int m_Frames;
    };

    struct radEmBufferShadow
    {
        const void * m_pData;
        ALenum m_Format;
        ALsizei m_Size;
        ALsizei m_Freq;

        //
        // Streaming (queue) mode state. Emscripten's OpenAL cannot update a
        // buffer attached to a playing source, so the DirectSound-style
        // looping ring buffer is emulated by queueing each written region as
        // its own AL buffer and tracking the ring playback position across
        // dequeued chunks.
        //
        ALuint m_StreamSource;
        unsigned int m_LastMapOffset;
        unsigned int m_LastMapBytes;
        bool m_SuppressQueue;
        bool m_WantPlaying;
        unsigned long long m_DequeuedFrames;
        std::deque< radEmQueuedChunk > m_Queued;

        radEmBufferShadow( )
            : m_pData( NULL ), m_Format( 0 ), m_Size( 0 ), m_Freq( 0 ),
              m_StreamSource( 0 ), m_LastMapOffset( 0 ), m_LastMapBytes( 0 ),
              m_SuppressQueue( false ), m_WantPlaying( false ),
              m_DequeuedFrames( 0 )
        {
        }
    };

    // Buffer loads can be dispatched on the drive thread as well as the
    // main thread, so guard the shadow map.
    pthread_mutex_t s_EmBufferShadowLock = PTHREAD_MUTEX_INITIALIZER;
    std::map< ALuint, radEmBufferShadow > s_EmBufferShadows;

    unsigned int radEmFormatFrameBytes( ALenum format )
    {
        switch ( format )
        {
            case AL_FORMAT_MONO8:    return 1;
            case AL_FORMAT_MONO16:   return 2;
            case AL_FORMAT_STEREO8:  return 2;
            case AL_FORMAT_STEREO16: return 4;
            default:                 return 1;
        }
    }

    // Reclaims chunks the source has finished playing. Lock must be held.
    void radEmStreamReclaim( radEmBufferShadow & shadow )
    {
        if ( shadow.m_StreamSource == 0 )
        {
            return;
        }

        ALint processed = 0;
        alGetSourcei( shadow.m_StreamSource, AL_BUFFERS_PROCESSED, &processed );
        alGetError( );

        while ( processed-- > 0 && !shadow.m_Queued.empty( ) )
        {
            ALuint chunkBuffer = 0;
            alSourceUnqueueBuffers( shadow.m_StreamSource, 1, &chunkBuffer );
            if ( alGetError( ) != AL_NO_ERROR || chunkBuffer == 0 )
            {
                break;
            }

            shadow.m_DequeuedFrames += shadow.m_Queued.front( ).m_Frames;
            shadow.m_Queued.pop_front( );
            alDeleteBuffers( 1, &chunkBuffer );
            alGetError( );
        }
    }

    // Stops the source and discards all queued chunks. Lock must be held.
    void radEmStreamFlush( radEmBufferShadow & shadow )
    {
        if ( shadow.m_StreamSource == 0 )
        {
            return;
        }

        alSourceStop( shadow.m_StreamSource );
        alSourcei( shadow.m_StreamSource, AL_BUFFER, 0 );
        alGetError( );

        while ( !shadow.m_Queued.empty( ) )
        {
            ALuint chunkBuffer = shadow.m_Queued.front( ).m_Buffer;
            shadow.m_Queued.pop_front( );
            alDeleteBuffers( 1, &chunkBuffer );
        }
        alGetError( );

        shadow.m_DequeuedFrames = 0;
    }

    // Queues one written ring region as its own AL buffer. Lock must be held.
    void radEmStreamQueueRegion( radEmBufferShadow & shadow,
        unsigned int offset, unsigned int bytes )
    {
        if ( shadow.m_StreamSource == 0 || bytes == 0 )
        {
            return;
        }

        radEmStreamReclaim( shadow );

        ALuint chunkBuffer = 0;
        alGenBuffers( 1, &chunkBuffer );
        if ( alGetError( ) != AL_NO_ERROR || chunkBuffer == 0 )
        {
            return;
        }

        alBufferData( chunkBuffer, shadow.m_Format,
            static_cast< const char * >( shadow.m_pData ) + offset,
            bytes, shadow.m_Freq );
        alSourceQueueBuffers( shadow.m_StreamSource, 1, &chunkBuffer );
        if ( alGetError( ) != AL_NO_ERROR )
        {
            alDeleteBuffers( 1, &chunkBuffer );
            alGetError( );
            return;
        }

        radEmQueuedChunk chunk;
        chunk.m_Buffer = chunkBuffer;
        chunk.m_Frames = bytes / radEmFormatFrameBytes( shadow.m_Format );
        shadow.m_Queued.push_back( chunk );

        if ( shadow.m_WantPlaying )
        {
            ALint state = 0;
            alGetSourcei( shadow.m_StreamSource, AL_SOURCE_STATE, &state );
            alGetError( );
            if ( state != AL_PLAYING )
            {
                alSourcePlay( shadow.m_StreamSource );
                alGetError( );
            }
        }
    }

    void AL_APIENTRY radEmBufferStorage(
        ALuint buffer, ALenum format, const ALvoid * data,
        ALsizei size, ALsizei freq, ALbitfieldSOFT flags )
    {
        (void) flags;
        pthread_mutex_lock( &s_EmBufferShadowLock );
        radEmBufferShadow & shadow = s_EmBufferShadows[ buffer ];
        shadow.m_pData = data;
        shadow.m_Format = format;
        shadow.m_Size = size;
        shadow.m_Freq = freq;
        pthread_mutex_unlock( &s_EmBufferShadowLock );
    }

    void * AL_APIENTRY radEmMapBuffer(
        ALuint buffer, ALsizei offset, ALsizei length, ALbitfieldSOFT access )
    {
        (void) access;
        pthread_mutex_lock( &s_EmBufferShadowLock );
        std::map< ALuint, radEmBufferShadow >::iterator it = s_EmBufferShadows.find( buffer );
        void * p = NULL;
        if ( it != s_EmBufferShadows.end( ) )
        {
            it->second.m_LastMapOffset = ( unsigned int ) offset;
            it->second.m_LastMapBytes = ( unsigned int ) length;
            p = const_cast< char * >( static_cast< const char * >( it->second.m_pData ) ) + offset;
        }
        pthread_mutex_unlock( &s_EmBufferShadowLock );
        return p;
    }

    void AL_APIENTRY radEmUnmapBuffer( ALuint buffer )
    {
        pthread_mutex_lock( &s_EmBufferShadowLock );
        std::map< ALuint, radEmBufferShadow >::iterator it = s_EmBufferShadows.find( buffer );
        if ( it != s_EmBufferShadows.end( ) && !it->second.m_SuppressQueue )
        {
            radEmStreamQueueRegion( it->second,
                it->second.m_LastMapOffset, it->second.m_LastMapBytes );
        }
        pthread_mutex_unlock( &s_EmBufferShadowLock );
    }
}

// Called from radSoundHalBufferWin's destructor so a stale entry can't hold
// a dangling pointer to the freed memory object.
void radEmForgetBuffer( ALuint buffer )
{
    pthread_mutex_lock( &s_EmBufferShadowLock );
    std::map< ALuint, radEmBufferShadow >::iterator it = s_EmBufferShadows.find( buffer );
    if ( it != s_EmBufferShadows.end( ) )
    {
        radEmStreamFlush( it->second );
        s_EmBufferShadows.erase( it );
    }
    pthread_mutex_unlock( &s_EmBufferShadowLock );
}

//
// Streaming voice hooks (called from radSoundHalVoiceWin / radSoundHalBufferWin).
//

void radEmStreamAttach( ALuint buffer, ALuint source )
{
    pthread_mutex_lock( &s_EmBufferShadowLock );
    radEmBufferShadow & shadow = s_EmBufferShadows[ buffer ];
    if ( shadow.m_StreamSource != source )
    {
        radEmStreamFlush( shadow );
        shadow.m_StreamSource = source;
    }
    shadow.m_WantPlaying = false;
    pthread_mutex_unlock( &s_EmBufferShadowLock );
}

void radEmStreamDetach( ALuint buffer )
{
    pthread_mutex_lock( &s_EmBufferShadowLock );
    std::map< ALuint, radEmBufferShadow >::iterator it = s_EmBufferShadows.find( buffer );
    if ( it != s_EmBufferShadows.end( ) )
    {
        radEmStreamFlush( it->second );
        it->second.m_StreamSource = 0;
        it->second.m_WantPlaying = false;
    }
    pthread_mutex_unlock( &s_EmBufferShadowLock );
}

void radEmStreamPlay( ALuint buffer )
{
    pthread_mutex_lock( &s_EmBufferShadowLock );
    std::map< ALuint, radEmBufferShadow >::iterator it = s_EmBufferShadows.find( buffer );
    if ( it != s_EmBufferShadows.end( ) )
    {
        it->second.m_WantPlaying = true;
        if ( it->second.m_StreamSource != 0 && !it->second.m_Queued.empty( ) )
        {
            ALint state = 0;
            alGetSourcei( it->second.m_StreamSource, AL_SOURCE_STATE, &state );
            alGetError( );
            if ( state != AL_PLAYING )
            {
                alSourcePlay( it->second.m_StreamSource );
                alGetError( );
            }
        }
    }
    pthread_mutex_unlock( &s_EmBufferShadowLock );
}

void radEmStreamStop( ALuint buffer )
{
    pthread_mutex_lock( &s_EmBufferShadowLock );
    std::map< ALuint, radEmBufferShadow >::iterator it = s_EmBufferShadows.find( buffer );
    if ( it != s_EmBufferShadows.end( ) )
    {
        it->second.m_WantPlaying = false;
        radEmStreamFlush( it->second );
    }
    pthread_mutex_unlock( &s_EmBufferShadowLock );
}

bool radEmStreamIsPlaying( ALuint buffer )
{
    bool playing = false;
    pthread_mutex_lock( &s_EmBufferShadowLock );
    std::map< ALuint, radEmBufferShadow >::iterator it = s_EmBufferShadows.find( buffer );
    if ( it != s_EmBufferShadows.end( ) && it->second.m_StreamSource != 0 )
    {
        ALint state = 0;
        alGetSourcei( it->second.m_StreamSource, AL_SOURCE_STATE, &state );
        alGetError( );
        playing = ( state == AL_PLAYING ) ||
            ( it->second.m_WantPlaying && !it->second.m_Queued.empty( ) );
    }
    pthread_mutex_unlock( &s_EmBufferShadowLock );
    return playing;
}

// Emulates AL_SAMPLE_OFFSET of the original looping ring buffer: position in
// PCM frames within the ring, advancing across queued chunks.
unsigned int radEmStreamGetPositionFrames( ALuint buffer )
{
    unsigned int position = 0;
    pthread_mutex_lock( &s_EmBufferShadowLock );
    std::map< ALuint, radEmBufferShadow >::iterator it = s_EmBufferShadows.find( buffer );
    if ( it != s_EmBufferShadows.end( ) && it->second.m_StreamSource != 0 )
    {
        radEmStreamReclaim( it->second );

        ALint offset = 0;
        alGetSourcei( it->second.m_StreamSource, AL_SAMPLE_OFFSET, &offset );
        alGetError( );

        unsigned int ringFrames = ( unsigned int )( it->second.m_Size /
            radEmFormatFrameBytes( it->second.m_Format ) );
        if ( ringFrames > 0 )
        {
            position = ( unsigned int )
                ( ( it->second.m_DequeuedFrames + ( unsigned long long ) offset ) % ringFrames );
        }
    }
    pthread_mutex_unlock( &s_EmBufferShadowLock );
    return position;
}

void radEmStreamResetPosition( ALuint buffer )
{
    pthread_mutex_lock( &s_EmBufferShadowLock );
    std::map< ALuint, radEmBufferShadow >::iterator it = s_EmBufferShadows.find( buffer );
    if ( it != s_EmBufferShadows.end( ) )
    {
        radEmStreamFlush( it->second );
    }
    pthread_mutex_unlock( &s_EmBufferShadowLock );
}

bool radEmBufferIsStream( ALuint buffer )
{
    pthread_mutex_lock( &s_EmBufferShadowLock );
    std::map< ALuint, radEmBufferShadow >::iterator it = s_EmBufferShadows.find( buffer );
    bool isStream = ( it != s_EmBufferShadows.end( ) && it->second.m_StreamSource != 0 );
    pthread_mutex_unlock( &s_EmBufferShadowLock );
    return isStream;
}

void radEmBufferSetQueueSuppressed( ALuint buffer, bool suppressed )
{
    pthread_mutex_lock( &s_EmBufferShadowLock );
    std::map< ALuint, radEmBufferShadow >::iterator it = s_EmBufferShadows.find( buffer );
    if ( it != s_EmBufferShadows.end( ) )
    {
        it->second.m_SuppressQueue = suppressed;
    }
    pthread_mutex_unlock( &s_EmBufferShadowLock );
}
#endif // __EMSCRIPTEN__

//================================================================================
// Static Members
//================================================================================

radSoundHalSystem * radSoundHalSystem::s_pRsdSystem = NULL;
static int g_RadSoundInitializeCount = 0;

//============================================================================
// radSoundHalSystem::radSoundHalSystem
//============================================================================

radSoundHalSystem::radSoundHalSystem( radMemoryAllocator allocator )
    :
    m_NumAuxSends( 0 ),
    m_pSoundMemory( 0 ),
    m_LastServiceTime( ::radTimeGetMilliseconds( ) )
{
    s_pRsdSystem = this;

    for( unsigned int i = 0; i < RSD_SYSTEM_MAX_AUX_SENDS; i++ )
    {
        m_refIRadSoundHalEffect[ i ] = NULL;
    }
    
	::radSoundPrintBanner( );
}

//============================================================================
// radSoundHalSystem::~radSoundHalSystem
//============================================================================

radSoundHalSystem::~radSoundHalSystem( void )
{
	radSoundHalListener::Terminate( );

    if (m_NumAuxSends > 0)
        alDeleteAuxiliaryEffectSlots(m_NumAuxSends, m_AuxSlots);

    alcMakeContextCurrent(NULL);
    if (m_pContext)
        alcDestroyContext(m_pContext);
    m_pContext = NULL;

    if (m_pDevice)
        alcCloseDevice(m_pDevice);
    m_pDevice = NULL;

	radSoundHalMemoryRegion::Terminate( );
    ::radMemoryFreeAligned( GetThisAllocator( ), m_pSoundMemory );

    s_pRsdSystem = NULL;
}

typedef void (AL_APIENTRY*ALDEBUGPROCEXT)(ALenum source, ALenum type, ALuint id, ALenum severity, ALsizei length, const ALchar *message, void *userParam);

typedef void (AL_APIENTRY*LPALDEBUGMESSAGECALLBACKEXT)(ALDEBUGPROCEXT callback, void *userParam);

void AL_APIENTRY PrintOpenALErrors(ALenum source, ALenum type, ALuint id, ALenum severity, ALsizei length, const ALchar *message, void *userParam)
{
    (void)length;
    (void)userParam;
    fprintf(stderr, "OpenAL says: source=%u type=%u id=%u severity=%u '%s'\n", source, type, id, severity, message);
}

//============================================================================
// radSoundHalSystem::Initialize
//============================================================================

void radSoundHalSystem::Initialize( const SystemDescription & systemDescription )
{
    rAssertMsg( systemDescription.m_SamplingRate != 0, 
        "ERROR radsound: system sampling rate must be set"
        "to the highest sampling rate required by your program (probably 48000Hz)" );

    m_NumAuxSends = systemDescription.m_NumAuxSends;

    // Initialize OpenAL

    m_pDevice = alcOpenDevice(NULL);

    ALenum err = alcGetError(m_pDevice);
    rAssertMsg(err == AL_NO_ERROR, "OpenAL device couldn't be opened.");

    if (err == AL_NO_ERROR)
    {
        //
        // Setup the primary context.
        //

        ALCint attr[] = {
            ALC_FREQUENCY, (ALCint)systemDescription.m_SamplingRate,
            ALC_MAX_AUXILIARY_SENDS, m_NumAuxSends,
            0
        };
        m_pContext = alcCreateContext(m_pDevice, attr);
        if (m_pContext == NULL)
        {
            // Some implementations (e.g. Emscripten's) reject the
            // ALC_MAX_AUXILIARY_SENDS attribute; retry with the frequency only.
            alcGetError(m_pDevice);
            ALCint freqOnlyAttr[] = {
                ALC_FREQUENCY, (ALCint)systemDescription.m_SamplingRate,
                0
            };
            m_pContext = alcCreateContext(m_pDevice, freqOnlyAttr);
        }

        ALenum err = alcGetError(m_pDevice);
        rAssertMsg(err == AL_NO_ERROR, "OpenAL context couldn't be created.");

        if (err == AL_NO_ERROR)
        {
            alcMakeContextCurrent(m_pContext);

#ifdef __EMSCRIPTEN__
            radBufferStorageSOFT = radEmBufferStorage;
            radMapBufferSOFT = radEmMapBuffer;
            radUnmapBufferSOFT = radEmUnmapBuffer;
#else
            rAssert( alIsExtensionPresent( "AL_SOFTX_map_buffer" ) );

            radBufferStorageSOFT = (LPALBUFFERSTORAGESOFT)alGetProcAddress( "alBufferStorageSOFT" );
            radMapBufferSOFT = (LPALMAPBUFFERSOFT)alGetProcAddress( "alMapBufferSOFT" );
            radUnmapBufferSOFT = (LPALUNMAPBUFFERSOFT)alGetProcAddress( "alUnmapBufferSOFT" );
#endif

            // enable debug messages, as of OpenAL-Soft v1.23.1 this extension has not been released yet
            if (alIsExtensionPresent("AL_EXT_debug"))
            {
                auto const AL_DEBUG_OUTPUT_EXT = alGetEnumValue("AL_DEBUG_OUTPUT_EXT");
                auto const alDebugMessageCallbackEXT = (LPALDEBUGMESSAGECALLBACKEXT)alGetProcAddress("alDebugMessageCallbackEXT");
                alEnable(AL_DEBUG_OUTPUT_EXT);
                alDebugMessageCallbackEXT(PrintOpenALErrors, /*userParam*/nullptr);
            }

            if (m_NumAuxSends > 0 && alcIsExtensionPresent(m_pDevice, "ALC_EXT_EFX"))
            {
                alGenAuxiliaryEffectSlots = (LPALGENAUXILIARYEFFECTSLOTS)alGetProcAddress("alGenAuxiliaryEffectSlots");
                alDeleteAuxiliaryEffectSlots = (LPALDELETEAUXILIARYEFFECTSLOTS)alGetProcAddress("alDeleteAuxiliaryEffectSlots");
                alAuxiliaryEffectSlotf = (LPALAUXILIARYEFFECTSLOTF)alGetProcAddress("alAuxiliaryEffectSlotf");
                alGetAuxiliaryEffectSlotf = (LPALGETAUXILIARYEFFECTSLOTF)alGetProcAddress("alGetAuxiliaryEffectSlotf");

                alcGetIntegerv(m_pDevice, ALC_MAX_AUXILIARY_SENDS, 1, &m_NumAuxSends);
                alGenAuxiliaryEffectSlots(m_NumAuxSends, m_AuxSlots);
            }
            else
            {
                m_NumAuxSends = 0;
            }
        }
    }

    radSoundHalListener::Initialize
	(
		GetThisAllocator( ),
        m_pContext
	);

    // Allocate memory

    m_pSoundMemory = ::radMemoryAllocAligned( 
        GetThisAllocator( ),
        systemDescription.m_ReservedSoundMemory, 
        radSoundHalDataSourceReadAlignmentGet( ) );

    radSoundHalMemoryRegion::Initialize( 
        m_pSoundMemory, 
        systemDescription.m_ReservedSoundMemory, 
        systemDescription.m_MaxRootAllocations,
        radSoundHalDataSourceReadAlignmentGet( ), 
        radMemorySpace_Local, GetThisAllocator( ) );
}

//============================================================================
// radSoundHalSystem::GetRootMemoryRegion
//============================================================================

IRadSoundHalMemoryRegion * radSoundHalSystem::GetRootMemoryRegion( void )
{
	return radSoundHalMemoryRegion::GetRootRegion( );
}

//============================================================================
// radSoundHalSystem::GetNumAuxSends
//============================================================================

unsigned int radSoundHalSystem::GetNumAuxSends( )
{
    return m_NumAuxSends;
}

//============================================================================
// radSoundHalSystem::SetOutputMode
//============================================================================

void radSoundHalSystem::SetOutputMode( radSoundOutputMode mode )
{
	rDebugString( "radSoundHalSystem: SetOutputMode() not supported on Win32/XBox use DashBoard\n" );
}

//============================================================================
// radSoundHalSystem::GetOutputMode
//============================================================================

radSoundOutputMode radSoundHalSystem::GetOutputMode( void )
{
	return radSoundOutputMode_Stereo;
}

//============================================================================
// radSoundHalSystem::Service
//============================================================================

void radSoundHalSystem::Service( void )
{
    unsigned int now = ::radTimeGetMilliseconds( );

    radSoundUpdatableObject::UpdateAll( now - m_LastServiceTime );

    m_LastServiceTime = now;
}

//============================================================================
// radSoundHalSystem::ServiceOncePerFrame
//============================================================================

void radSoundHalSystem::ServiceOncePerFrame( void )
{
	radSoundHalListener::GetInstance( )->UpdatePositionalSettings( );
}

//============================================================================
// radSoundHalSystem::GetStats
//============================================================================
    
void radSoundHalSystem::GetStats( IRadSoundHalSystem::Stats * pStats )
{
    rAssert( pStats );

    ::memset( pStats, 0, sizeof( IRadSoundHalSystem::Stats ) );

	//
	// Get voice info
	//

	radSoundHalVoiceWin * pVoiceSearch = radSoundHalVoiceWin::GetLinkedClassHead( );
		
    while ( pVoiceSearch != NULL )
    {
		if ( pVoiceSearch->GetPositionalGroup( ) != NULL )
		{
			pStats->m_NumPosVoices++;

			if ( pVoiceSearch->IsPlaying( ) )
			{
				pStats->m_NumPosVoicesPlaying++;
			}				
		}
		else
		{
			pStats->m_NumVoices++;

			if ( pVoiceSearch->IsPlaying( ) )
			{
				pStats->m_NumVoicesPlaying++;
			}
		}

        pVoiceSearch = pVoiceSearch->GetLinkedClassNext( );
    }

	//
	// GetBuffer info
	//
	
	radSoundHalBufferWin * pBufferSearch = radSoundHalBufferWin::GetLinkedClassHead( );

	while ( pBufferSearch != NULL )
	{
		pStats->m_NumBuffers ++;
		pStats->m_BufferMemoryUsed += pBufferSearch->GetSizeInBytes( );
		
		pBufferSearch = pBufferSearch->GetLinkedClassNext( );
	}
	
	// Effects Memory is always zero it is in the hardware.

	pStats->m_EffectsMemoryUsed = 0;
									
	radSoundHalMemoryRegion::GetRootRegion( )->GetStats( & pStats->m_TotalFreeSoundMemory, NULL, NULL, true );
}

//============================================================================
// radSoundHalSystem::SetAuxEffect
//============================================================================

void radSoundHalSystem::SetAuxEffect( unsigned int auxNumber, IRadSoundHalEffect * pIRadSoundHalEffect )
{
    rAssert( auxNumber < m_NumAuxSends );

    if( m_refIRadSoundHalEffect[ auxNumber ] != NULL )
    {
        m_refIRadSoundHalEffect[ auxNumber ]->Detach( );
    }

    m_refIRadSoundHalEffect[ auxNumber ] = pIRadSoundHalEffect;

    if( m_refIRadSoundHalEffect[ auxNumber ] != NULL )
    {
        m_refIRadSoundHalEffect[ auxNumber ]->Attach( auxNumber );
    }
}

//============================================================================
// radSoundHalSystem::GetAuxEffect
//============================================================================

IRadSoundHalEffect * radSoundHalSystem::GetAuxEffect( unsigned int auxNumber )
{
    rAssert( auxNumber < m_NumAuxSends );
    return m_refIRadSoundHalEffect[ auxNumber ];
}

//============================================================================
// radSoundHalSystem::SetAuxGain
//============================================================================

void radSoundHalSystem::SetAuxGain( unsigned int aux, float gain )
{
    rAssert(aux < m_NumAuxSends);
    alAuxiliaryEffectSlotf(m_AuxSlots[aux], AL_EFFECTSLOT_GAIN, gain);
    rAssert(alGetError() == AL_NO_ERROR);
}

//============================================================================
// radSoundHalSystem::GetAuxGain
//============================================================================

float radSoundHalSystem::GetAuxGain( unsigned int aux )
{
    rAssert(aux < m_NumAuxSends);
    rWarningMsg( false, "system::GetAuxGain not supported on PC" );
    ALfloat gain;
    alGetAuxiliaryEffectSlotf(m_AuxSlots[aux], AL_EFFECTSLOT_GAIN, &gain);
    rAssert(alGetError() == AL_NO_ERROR);
    return gain;
}

//============================================================================
// radSoundHalSystem::GetOpenALDevice
//============================================================================

ALCdevice * radSoundHalSystem::GetOpenALDevice( void )
{
    return m_pDevice;
}

//============================================================================
// radSoundHalSystem::GetOpenALContext
//============================================================================

ALCcontext * radSoundHalSystem::GetOpenALContext( void )
{
    return m_pContext;
}

//============================================================================
// radSoundHalSystem::GetContext
//============================================================================

ALuint radSoundHalSystem::GetOpenALAuxSlot( unsigned int aux )
{
    rAssert(aux < m_NumAuxSends);

    return m_AuxSlots[aux];
}

//============================================================================
// radSoundHalSystem::GetInstance
//============================================================================

radSoundHalSystem * radSoundHalSystem::GetInstance( void )
{
    return s_pRsdSystem;
}

//================================================================================
// ::rsdGetSystem
//================================================================================

IRadSoundHalSystem * radSoundHalSystemGet( void )
{
    rAssert( radSoundHalSystem::s_pRsdSystem != NULL );

    return radSoundHalSystem::s_pRsdSystem;
}

//================================================================================
// ::radSoundIntialize
//================================================================================

void radSoundHalSystemInitialize( radMemoryAllocator allocator  )
{
    rAssert( radSoundHalSystem::s_pRsdSystem == NULL );

    new( "radSoundHalSystem", allocator ) radSoundHalSystem( allocator );
    radSoundHalSystem::s_pRsdSystem->AddRef( );
}

//================================================================================
// ::radSoundIntialize
//================================================================================
        
void radSoundHalSystemTerminate( void )
{
    rAssert( radSoundHalSystem::s_pRsdSystem != NULL );

    radSoundHalSystem::s_pRsdSystem->Release( );
}











   
