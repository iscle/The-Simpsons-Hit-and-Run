//=============================================================================
// Copyright (c) 2002 Radical Games Ltd.  All rights reserved.
//=============================================================================


//=============================================================================
//
// File:        nullmovieplayer.cpp
//
// Subsystem:	Foundation Technologies - Movie Player
//
// Description:	Stub movie player for platforms without a video decoder
//              (e.g. the Emscripten/browser build). Movies report completion
//              immediately so game flow that waits on FMV playback proceeds.
//
//=============================================================================

//=============================================================================
// Include Files
//=============================================================================

#include <radoptions.hpp>

#ifdef RAD_MOVIEPLAYER_NULL

#include <raddebug.hpp>
#include <radobject.hpp>
#include <radmovie2.hpp>

//=============================================================================
// Constants
//=============================================================================

const char * radMovieDebugChannel2 = "radMovie";
unsigned int const radMovie_NoAudioTrack = 0xFFFFFFFF;

//=============================================================================
// Class radNullMoviePlayer
//=============================================================================

class radNullMoviePlayer
    :
    public IRadMoviePlayer2,
    public radRefCount
{
    public:

        IMPLEMENT_REFCOUNTED( "radNullMoviePlayer" )

        radNullMoviePlayer( void )
            : m_State( NoData ), m_Volume( 1.0f ), m_Pan( 0.0f )
        {
        }

        virtual ~radNullMoviePlayer( void )
        {
        }

        virtual void Initialize(
            IRadMovieRenderLoop * pIRadMovieRenderLoop,
            IRadMovieRenderStrategy * pIRadMovieRenderStrategy )
        {
            (void) pIRadMovieRenderLoop;
            (void) pIRadMovieRenderStrategy;
        }

        virtual bool Render( void )
        {
            return false;
        }

        virtual void Load( const char * pVideoFileName, unsigned int audioTrackIndex )
        {
            (void) pVideoFileName;
            (void) audioTrackIndex;

            m_State = ReadyToPlay;
        }

        virtual void Unload( void )
        {
            m_State = NoData;
        }

        virtual void Play( void )
        {
            // The movie "finishes" instantly; clients poll for NoData to
            // detect the end of playback.
            m_State = NoData;
        }

        virtual void Pause( void )
        {
            if( m_State == Playing || m_State == LoadToPlay )
            {
                m_State = ReadyToPlay;
            }
        }

        virtual void SetVolume( float volume ) { m_Volume = volume; }
        virtual float GetVolume( void ) { return m_Volume; }
        virtual void SetPan( float pan ) { m_Pan = pan; }
        virtual float GetPan( void ) { return m_Pan; }

        virtual State GetState( void )
        {
            return m_State;
        }

        virtual bool GetVideoFrameInfo( VideoFrameInfo * pFrameInfo )
        {
            if( m_State == NoData )
            {
                return false;
            }

            pFrameInfo->Width = 640;
#ifdef PAL
            pFrameInfo->Height = 528;
#else
            pFrameInfo->Height = 480;
#endif
            return true;
        }

        virtual float GetFrameRate( void )
        {
            return 30.0f;
        }

        virtual unsigned int GetCurrentFrameNumber( void )
        {
            return 0;
        }

    private:

        State m_State;
        float m_Volume;
        float m_Pan;
};

//=============================================================================
// Function:    radMoviePlayerCreate2
//=============================================================================

IRadMoviePlayer2 * radMoviePlayerCreate2( radMemoryAllocator alloc )
{
    return new( alloc ) radNullMoviePlayer( );
}

//=============================================================================
// Function:    radMovieInitialize2
//=============================================================================

void radMovieInitialize2( radMemoryAllocator alloc )
{
    (void) alloc;
}

//=============================================================================
// Function:    radMovieTerminate2
//=============================================================================

void radMovieTerminate2( void )
{
}

//=============================================================================
// Function:    radMovieService2
//=============================================================================

void radMovieService2( void )
{
}

#endif // RAD_MOVIEPLAYER_NULL
