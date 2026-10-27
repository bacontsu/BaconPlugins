#-----------------------------------------------------------------------------
#	vpLightBake
#
#	Project Script
#-----------------------------------------------------------------------------

cmake_minimum_required( VERSION 3.20 FATAL_ERROR )

include( "${SRCDIR}/sdk/JackPluginSDK.cmake" )

set( LIGHTBAKE_SOURCE_FILES )
BEGIN_SRC( LIGHTBAKE_SOURCE_FILES "Source Files" )
	SRC_GRP(
		SOURCES
		#{
			"actions.cpp"
			"main.cpp"
			"map.cpp"
			"preview.cpp"
			"rad.cpp"
			"wad.cpp"

			"../common/vstdlib_static.cpp"
		#}
	)
END_SRC( LIGHTBAKE_SOURCE_FILES "Source Files" )

set( LIGHTBAKE_HEADER_FILES )
BEGIN_SRC( LIGHTBAKE_HEADER_FILES "Header Files" )
	SRC_GRP(
		SOURCES
		#{
			"lightbake.h"
			"wad.h"

			"../common/vstdlib_static.h"
		#}
	)
END_SRC( LIGHTBAKE_HEADER_FILES "Header Files" )

add_library(
	lightbake MODULE
	${LIGHTBAKE_SOURCE_FILES} ${LIGHTBAKE_HEADER_FILES}
)

install_library(
	TARGET lightbake
	INSTALL_OUTNAME "vpLightBake"
	INSTALL_DEST "${CMAKE_INSTALL_PREFIX}"
)

target_include_directories(
	lightbake PRIVATE

	"."
	"../common"
)

target_use_jackpluginsdk( lightbake "${SRCDIR}/sdk" )
