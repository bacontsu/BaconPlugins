//=====================================================================================//
// bacontsu: entry points
//=====================================================================================//
#include <stdio.h>
#include <stdlib.h>
#include <locale.h>

#if defined( WIN32 )
#include <windows.h>
#endif // WIN32

#include "PluginMeta.h"

#include "lightbake.h"

plugin_funcs_t gEditorfuncs;

CTextureCache g_textureCache;

#if defined( WIN32 )
// bacontsu: the mouse hook must never outlive the module that owns its callback
BOOL APIENTRY DllMain( HMODULE moduleHandle, DWORD reason, LPVOID reserved )
{
	if ( reason == DLL_PROCESS_DETACH )
		LB_InstallAutoRestoreHook( false );

	return TRUE;
}
#endif // WIN32

// bacontsu: API handshake goes to %TEMP%\vpLightBake.log, a rejected plugin leaves no other trace
static void LB_LogPluginLoad( plugin_funcs_t *editorFuncs, int editorPluginVersion )
{
	const char *tempDir = getenv( "TEMP" );

	if ( !tempDir )
		tempDir = getenv( "TMP" );

	if ( !tempDir )
		tempDir = ".";

	char path[MAX_PATH];
	snprintf( path, sizeof( path ), "%s\\vpLightBake.log", tempDir );

	FILE *f = fopen( path, "a" );

	if ( !f )
		return;

	fprintf( f, "vpMain: editorApi=%d editorInterfaceSize=%d pluginInterfaceSize=%d pluginApi=%d\n",
		editorPluginVersion,
		editorFuncs ? (int)editorFuncs->m_intefaceVersion : -1,
		(int)sizeof( plugin_funcs_t ),
		(int)PLUGIN_VERSION );

	if ( editorFuncs && editorFuncs->m_intefaceVersion != sizeof( plugin_funcs_t ) )
		fprintf( f, "vpMain: NOTE interface sizes differ, copying min( editor, plugin )\n" );

	fclose( f );
}

DLL_EXPORT int vpMain( plugin_funcs_t *editorFuncs, int editorPluginVersion )
{
	LB_LogPluginLoad( editorFuncs, editorPluginVersion );

	if ( !editorFuncs )
		return -1;

	if ( editorFuncs->m_intefaceVersion < sizeof( plugin_funcs_t ) )
		return -1;

	if ( editorPluginVersion != PLUGIN_VERSION )
		return PLUGIN_VERSION;

	// never copy more than our own table can hold: the editor's struct may be larger
	// than the one this plugin was built against
	size_t copySize = editorFuncs->m_intefaceVersion;

	if ( copySize > sizeof( plugin_funcs_t ) )
		copySize = sizeof( plugin_funcs_t );

	memcpy( &gEditorfuncs, editorFuncs, copySize );

	setlocale( LC_ALL, "C" );
	return 0;
}
