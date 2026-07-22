set(RADSOUND_SOURCES
	"src/common/radsoundobject.cpp"
	"src/common/radsoundupdatableobject.cpp"
	"src/hal/common/audioformat.cpp"
	"src/hal/common/banner.cpp"
	"src/hal/common/memoryregion.cpp"
	"src/hal/common/radsoundfile.cpp"
	"src/hal/common/rolloff.cpp"
	"src/hal/win32/buffer.cpp"
	"src/hal/win32/bufferloader.cpp"
	"src/hal/win32/effect.cpp"
	"src/hal/win32/listener.cpp"
	"src/hal/win32/positionalgroup.cpp"
	"src/hal/win32/radsoundwin.cpp"
	"src/hal/win32/system.cpp"
	"src/hal/win32/voice.cpp"
	"src/math/radsoundmath.cpp"
	"src/pch/pch.cpp"
	"src/radsound/buffereddatasource.cpp"
	"src/radsound/clip.cpp"
	"src/radsound/clipplayer.cpp"
	"src/radsound/datacache.cpp"
	"src/radsound/memorydatasource.cpp"
	"src/radsound/memoryspaceobject.cpp"
	"src/radsound/radicaladpcm.cpp"
	"src/radsound/rsdfiledatasource.cpp"
	"src/radsound/stitcheddatasource.cpp"
	"src/radsound/streamplayer.cpp"
)

if(SRR2_BUILD_TESTS)
	add_executable(simplesound "sample/simplesound/src/main.cpp")
	target_link_libraries(simplesound
		radsound
		Threads::Threads
	)
	file(COPY "sample/simplesound/bin/win/sample.rsd"
        DESTINATION "${CMAKE_CURRENT_BINARY_DIR}")
	file(COPY "sample/simplesound/bin/win/stream.rsd"
        DESTINATION "${CMAKE_CURRENT_BINARY_DIR}")
	if(VITA)
		vita_create_self(simplesound.self simplesound)
	endif()
endif()

add_library(radsound STATIC ${RADSOUND_SOURCES})
target_include_directories(radsound
	PUBLIC "inc/"
	PRIVATE "src/pch/"
	PRIVATE "src/common/"
	PRIVATE ${OPENAL_INCLUDE_DIR}
)
if(EMSCRIPTEN)
	# Emscripten's OpenAL ships no efx.h; use the vendored openal-soft header.
	target_include_directories(radsound PRIVATE "src/hal/openal-compat/")
endif()
target_link_libraries(radsound
	${OPENAL_LIBRARY}
	radcore
)
