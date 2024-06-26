//=============================================================================
// Copyright (C) 2002 Radical Entertainment Ltd.  All rights reserved.
//
// File:        positionalsoundsettings.h
//
// Description: Declaration of object encapsulating positional sound settings
//
// History:     12/20/2002 + Created -- Darren
//
//=============================================================================

#ifndef POSITIONALSOUNDSETTINGS_H
#define POSITIONALSOUNDSETTINGS_H

//========================================
// Nested Includes
//========================================
#include <radlinkedclass.hpp>

#include <sound/soundfx/ipositionalsoundsettings.h>

//========================================
// Forward References
//========================================

//=============================================================================
//
// Synopsis:    positionalSoundSettings
//
//=============================================================================

class positionalSoundSettings: public IPositionalSoundSettings,
                               public radLinkedClass< positionalSoundSettings >,
                               public radRefCount
{
    public:
        IMPLEMENT_REFCOUNTED( "positionalSoundSettings" );
        positionalSoundSettings();
        virtual ~positionalSoundSettings();

        IPositionalSoundSettings& SetClipName( const char* clipName );
        const char* GetClipName() { return( m_clipName ); }

        IPositionalSoundSettings& SetMinDistance( float min );
        float GetMinDistance() { return( m_minDist ); }

        IPositionalSoundSettings& SetMaxDistance( float max );
        float GetMaxDistance() { return( m_maxDist ); }

        IPositionalSoundSettings& SetPlaybackProbability( float prob );
        float GetPlaybackProbability() { return( m_playProbability ); }

        //
        // Create a positionalSoundSettings object
        //
        static positionalSoundSettings* ObjCreate( radMemoryAllocator allocator );

    private:
        //Prevent wasteful constructor creation.
        positionalSoundSettings( const positionalSoundSettings& positionalsoundsettings );
        positionalSoundSettings& operator=( const positionalSoundSettings& positionalsoundsettings );

        //
        // Settings
        //
        char* m_clipName;
        float m_minDist;
        float m_maxDist;
        float m_playProbability;
};

#endif //POSITIONALSOUNDSETTINGS_H
