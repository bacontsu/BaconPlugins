//=====================================================================================//
// bacontsu: menu actions and the compile dialog
//=====================================================================================//
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <exception>

#if defined( WIN32 )
#include <windows.h>
#endif // WIN32

#include "lightbake.h"

extern CTextureCache g_textureCache;

// persisted between runs so the dialog remembers the last settings
static lbSettings_t g_settings;
static bool g_settingsLoaded = false;

static char g_wadPath[MAX_PATH] = { 0 };

#if defined( WIN32 )
static HHOOK s_mouseHook = NULL;
static bool s_restoringOnClick = false;

// bacontsu: sdk has no viewport callback, a left click is the best hint we get
static LRESULT CALLBACK LB_MouseHookProc( int nCode, WPARAM wParam, LPARAM lParam )
{
	if ( nCode >= 0 && wParam == WM_LBUTTONDOWN && !s_restoringOnClick && LB_HasPreview() )
	{
		const MOUSEHOOKSTRUCT *info = (const MOUSEHOOKSTRUCT *)lParam;

		DWORD processId = 0;

		if ( info && info->hwnd )
			GetWindowThreadProcessId( info->hwnd, &processId );

		if ( processId != 0 && processId == GetCurrentProcessId() )
		{
			s_restoringOnClick = true;

			LB_LogMsg( "auto restore: left click, putting the original textures back" );

			// keep the generated shaders: no GL context is guaranteed inside a mouse hook
			LB_RestorePreviewKeepShaders();

			s_restoringOnClick = false;
		}
	}

	return CallNextHookEx( s_mouseHook, nCode, wParam, lParam );
}
#endif // WIN32

void LB_InstallAutoRestoreHook( bool install )
{
#if defined( WIN32 )
	if ( install )
	{
		if ( !s_mouseHook )
		{
			s_mouseHook = SetWindowsHookExW( WH_MOUSE, LB_MouseHookProc, NULL, GetCurrentThreadId() );

			LB_LogMsg( "auto restore hook: %s", s_mouseHook ? "installed" : "FAILED" );
		}
	}
	else
	{
		if ( s_mouseHook )
		{
			UnhookWindowsHookEx( s_mouseHook );
			s_mouseHook = NULL;

			LB_LogMsg( "auto restore hook: removed" );
		}
	}
#else
	(void)install;
#endif // WIN32
}

void LB_LogMsg( const char *format, ... )
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

	va_list args;
	va_start( args, format );
	vfprintf( f, format, args );
	va_end( args );

	fputc( '\n', f );
	fclose( f );
}

// bacontsu: called from the compiler thread owner (never from a worker thread)
static void LB_Progress( int current, int total, const char *label )
{
	if ( total <= 0 )
		total = 1;

	Dialog_SetProgress( current, total );

	// only print when the phase changes, otherwise the log gets flooded
	static char lastLabel[128] = { 0 };

	if ( label && label[0] && strcmp( lastLabel, label ) )
	{
		strncpy( lastLabel, label, sizeof( lastLabel ) - 1 );
		lastLabel[sizeof( lastLabel ) - 1] = '\0';

		Dialog_Printf( "%s", label );
	}
}

// bacontsu: loads the WADs from the mod/base directory plus the user supplied path
static void LB_EnsureTextureCache( const char *extraDir )
{
	if ( g_textureCache.GetIndexedCount() > 0 && ( !extraDir || !extraDir[0] ) )
		return;

	char modDir[MAX_PATH] = { 0 };
	char baseDir[MAX_PATH] = { 0 };

	if ( Sys_GetModDirectory( modDir, sizeof( modDir ) ) )
	{
		LB_LogMsg( "wad: scanning mod dir '%s'", modDir );
		g_textureCache.AddDirectory( modDir, true );
	}

	if ( Sys_GetBaseDirectory( baseDir, sizeof( baseDir ) ) )
	{
		if ( !modDir[0] || stricmp( modDir, baseDir ) )
		{
			LB_LogMsg( "wad: scanning base dir '%s'", baseDir );
			g_textureCache.AddDirectory( baseDir, true );
		}
	}

	if ( extraDir && extraDir[0] )
	{
		LB_LogMsg( "wad: scanning user dir '%s'", extraDir );
		g_textureCache.AddDirectory( extraDir, true );
	}

	LB_LogMsg( "wad: %d textures indexed, %d decoded", g_textureCache.GetIndexedCount(), g_textureCache.GetCount() );

	Sys_Printf( "LightBake: %d textures indexed", g_textureCache.GetIndexedCount() );
}

// bacontsu: rad files sit next to the wads, info_texlights lives inside the map
static void LB_EnsureTexLights( const char *extraDir, qWorld_s *world )
{
	LB_ClearTexLights();

	char modDir[MAX_PATH] = { 0 };
	char baseDir[MAX_PATH] = { 0 };

	if ( Sys_GetModDirectory( modDir, sizeof( modDir ) ) )
		LB_LoadRadDirectory( modDir );

	if ( Sys_GetBaseDirectory( baseDir, sizeof( baseDir ) ) && ( !modDir[0] || stricmp( modDir, baseDir ) ) )
		LB_LoadRadDirectory( baseDir );

	if ( extraDir && extraDir[0] )
		LB_LoadRadDirectory( extraDir );

	LB_CollectInfoTexLights( world );

	LB_LogMsg( "texlights: %d textures known", LB_GetTexLightCount() );
}

// bacontsu: writes the geometry/lighting numbers of the first few faces to the log, so texture
static void LB_DumpDebugInfo( CMapGeometry &geometry )
{
	int count = (int)geometry.m_faces.size();

	if ( count > 8 )
		count = 8;

	for ( int i = 0; i < count; i++ )
	{
		lbFace_t &face = geometry.m_faces[i];

		if ( !face.m_lit || face.m_luxelLight.empty() )
			continue;

		vec3_t lightMin( 1e30f, 1e30f, 1e30f );
		vec3_t lightMax( -1e30f, -1e30f, -1e30f );
		vec3_t lightSum( 0.f, 0.f, 0.f );

		for ( size_t k = 0; k < face.m_luxelLight.size(); k++ )
		{
			const vec3_t &light = face.m_luxelLight[k];

			for ( int c = 0; c < 3; c++ )
			{
				if ( light[c] < lightMin[c] )
					lightMin[c] = light[c];
				if ( light[c] > lightMax[c] )
					lightMax[c] = light[c];

				lightSum[c] += light[c];
			}
		}

		float numLuxels = (float)face.m_luxelLight.size();

		if ( numLuxels < 1.f )
			numLuxels = 1.f;

		const qTexDef_t &texDef = face.m_face ? face.m_face->m_texDef : qTexDef_t();

		LB_LogMsg( "face[%d] tex='%s' texsize=%dx%d verts=%d lm=%dx%d chop=%.1f sky=%d occluder=%d", i, face.m_texName, face.m_texWidth,
			face.m_texHeight, (int)face.m_verts.size(), face.m_lmW, face.m_lmH, face.m_chop, face.m_sky ? 1 : 0, face.m_occluder ? 1 : 0 );

		LB_LogMsg( "   texdef U=(%.3f %.3f %.3f w=%.3f) V=(%.3f %.3f %.3f w=%.3f) scale=(%.3f %.3f) rot=%.2f align=%d", texDef.m_UAxis.x,
			texDef.m_UAxis.y, texDef.m_UAxis.z, texDef.m_UAxis.w, texDef.m_VAxis.x, texDef.m_VAxis.y, texDef.m_VAxis.z, texDef.m_VAxis.w,
			texDef.m_scale.x, texDef.m_scale.y, texDef.m_rotate, texDef.m_textureAlignment );

		if ( face.m_verts.size() > 0 && face.m_uv.size() > 0 )
		{
			LB_LogMsg( "   v0=(%.2f %.2f %.2f) uv0=(%.3f %.3f) gradU=(%.3f %.3f) gradV=(%.3f %.3f) off=(%.3f %.3f)", face.m_verts[0].x,
				face.m_verts[0].y, face.m_verts[0].z, face.m_uv[0].x, face.m_uv[0].y, face.m_uvGradU.x, face.m_uvGradU.y, face.m_uvGradV.x,
				face.m_uvGradV.y, face.m_uvOffset.x, face.m_uvOffset.y );
		}

		LB_LogMsg( "   bb=(%.1f %.1f)-(%.1f %.1f) light min=(%.1f %.1f %.1f) max=(%.1f %.1f %.1f) mean=(%.1f %.1f %.1f)", face.m_bbMin.x,
			face.m_bbMin.y, face.m_bbMax.x, face.m_bbMax.y, lightMin.x, lightMin.y, lightMin.z, lightMax.x, lightMax.y, lightMax.z,
			lightSum.x / numLuxels, lightSum.y / numLuxels, lightSum.z / numLuxels );
	}
}

// bacontsu: the whole compile, executed from the dialog's "Compile" button
static void LB_DoCompile()
{
	Dialog_Printf( "----------------------------------------" );

	LB_LogMsg( "=== compile requested ===" );

	qWorld_s *world = Global_GetCurrentWorld();

	if ( !world )
	{
		Dialog_Printf( "Error: no world loaded" );
		LB_LogMsg( "no world" );
		Dialog_MessageBox( "Compile Light", "No map is loaded.", DIALOG_MB_ICONERROR );
		return;
	}

	g_settings.m_bounceEnabled = Dialog_QueryArgumentInt( "bounceEnabled" ) != 0;
	g_settings.m_texLights = Dialog_QueryArgumentInt( "texLights" ) != 0;
	g_settings.m_bounce = Dialog_QueryArgumentInt( "bounce" );

	g_settings.m_chop = Dialog_QueryArgumentInt( "chop" );

	g_settings.m_lightScale = Dialog_QueryArgumentFloat( "brightness" );
	g_settings.m_lightGamma = Dialog_QueryArgumentFloat( "gamma" );
	g_settings.m_limitThreshold = ( Dialog_QueryArgumentInt( "limiter188" ) != 0 ) ? 188.f : 255.f;
	g_settings.m_skyScale = Dialog_QueryArgumentFloat( "sky" );

	g_settings.m_preview = Dialog_QueryArgumentInt( "preview" ) != 0;
	g_settings.m_restoreOnClick = Dialog_QueryArgumentInt( "restoreOnClick" ) != 0;
	g_settings.m_renameTextures = Dialog_QueryArgumentInt( "renameTextures" ) != 0;

	Dialog_QueryArgument( "wadpath", g_wadPath, sizeof( g_wadPath ) );

	if ( g_settings.m_chop < 4 )
		g_settings.m_chop = 4;

	if ( g_settings.m_bounce < 0 )
		g_settings.m_bounce = 0;

	if ( g_settings.m_lightScale <= 0.f )
		g_settings.m_lightScale = 1.f;

	LB_LogMsg( "settings: chop=%d bounce=%d preview=%d scale=%.2f gamma=%.2f limiter=%.1f sky=%.2f wads=%s",
		g_settings.m_chop, g_settings.m_bounce, g_settings.m_preview ? 1 : 0, g_settings.m_lightScale,
		g_settings.m_lightGamma, g_settings.m_limitThreshold, g_settings.m_skyScale, g_wadPath );

	// put the previous preview back BEFORE anything reads the world: otherwise the faces
	// still carry our generated texture names, the base texture lookup fails and the next
	// bake composites against mid grey (lightmap only, diffuse texture gone)
	if ( LB_HasPreview() )
	{
		LB_LogMsg( "restoring previous preview before rebuilding geometry" );
		LB_RestorePreview();
	}

	try
	{
		LB_EnsureTextureCache( g_wadPath );

		// texture lights need the wads for the albedo, so this runs after the cache
		if ( g_settings.m_texLights )
			LB_EnsureTexLights( g_wadPath, world );

		Dialog_SetProgress( 0, 1 );
		Dialog_Printf( "Loading geometry..." );

		LB_LogMsg( "loading geometry" );

		CMapGeometry geometry;

		if ( !geometry.Build( world ) )
		{
			Dialog_Printf( "Error: the map has no lit faces" );
			LB_LogMsg( "no lit faces" );
			Dialog_MessageBox( "Compile Light", "The map has no lit faces.", DIALOG_MB_ICONERROR );
			return;
		}

		LB_LogMsg( "faces=%d lit=%d occluderTris=%d lights=%d sky=%d", (int)geometry.m_faces.size(), geometry.m_numLitFaces,
			geometry.m_numOccluders, (int)geometry.m_lights.size(), geometry.m_sky.m_enabled ? 1 : 0 );

		if ( g_settings.m_texLights )
		{
			geometry.BuildEmitters( g_settings.m_texChop );

			if ( geometry.m_numTexLightFaces > 0 )
				Dialog_Printf( "%d texture light faces, %d emitters", geometry.m_numTexLightFaces, (int)geometry.m_emitters.size() );
		}

		geometry.BuildLightmaps( g_settings.m_chop );

		Dialog_Printf( "%d faces (%d lit), %d occluder triangles, %d lights", (int)geometry.m_faces.size(), geometry.m_numLitFaces,
			geometry.m_numOccluders, (int)geometry.m_lights.size() );

		if ( geometry.m_sky.m_enabled )
		{
			Dialog_Printf( "Sky lighting enabled, %d sky faces", geometry.m_numSkyFaces );

			LB_LogMsg( "sky: %d sky faces, sun=(%.0f %.0f %.0f) diffuse=(%.0f %.0f %.0f) dir=(%.2f %.2f %.2f)", geometry.m_numSkyFaces,
				geometry.m_sky.m_color.x, geometry.m_sky.m_color.y, geometry.m_sky.m_color.z, geometry.m_sky.m_diffuse.x,
				geometry.m_sky.m_diffuse.y, geometry.m_sky.m_diffuse.z, geometry.m_sky.m_direction.x, geometry.m_sky.m_direction.y,
				geometry.m_sky.m_direction.z );
		}
		else
		{
			LB_LogMsg( "sky: %d sky faces, no light_environment in the map", geometry.m_numSkyFaces );
		}

		Dialog_Printf( "%d lightmap samples, chop %d", geometry.m_numLuxels, (int)geometry.m_faces.empty() ? g_settings.m_chop : (int)geometry.m_faces[0].m_chop );

		LB_LogMsg( "luxels=%d", geometry.m_numLuxels );

		CRad rad( geometry, g_settings );

		if ( !rad.Run( LB_Progress ) )
		{
			Dialog_Printf( "Error: light compile failed" );
			LB_LogMsg( "compile failed" );
			return;
		}

		Dialog_Printf( "Direct lighting: %.2f seconds", rad.m_directTime );

		if ( g_settings.m_bounceEnabled && g_settings.m_bounce > 0 )
			Dialog_Printf( "Indirect lighting (%d bounces): %.2f seconds", g_settings.m_bounce, rad.m_bounceTime );

		LB_DumpDebugInfo( geometry );

		if ( g_settings.m_preview )
		{
			LB_LogMsg( "preview start" );

			if ( LB_BuildPreview( geometry, g_settings, LB_Progress ) )
			{
				Dialog_Printf( "Preview applied. Use \"Restore baked lighting\" to put the textures back." );

				if ( g_settings.m_renameTextures )
					Dialog_Printf( "WARNING: face texture names were changed. Restore before saving the map!" );
				else
					Dialog_Printf( "Face texture names are untouched, preview is safe to save." );
			}
			else
				Dialog_Printf( "Warning: preview could not be applied" );

			LB_LogMsg( "preview done" );
		}
	}
	catch ( const std::exception &e )
	{
		Dialog_Printf( "Error: %s", e.what() );
		LB_LogMsg( "exception: %s", e.what() );
		return;
	}
	catch ( ... )
	{
		Dialog_Printf( "Error: unknown exception while compiling" );
		LB_LogMsg( "unknown exception" );
		return;
	}

	Dialog_SetProgress( 1, 1 );
	Dialog_Printf( "Compile finished" );

	LB_LogMsg( "compile finished" );
}

// bacontsu: Plugin > Half-Life > Compile Light
static void LB_ActionCompile( int actionUserData )
{
	if ( !g_settingsLoaded )
	{
		LB_DefaultSettings( g_settings );
		g_settingsLoaded = true;
	}

	if ( !g_wadPath[0] )
		Sys_GetModDirectory( g_wadPath, sizeof( g_wadPath ) );

	Dialog_Begin( "Compile Light" );
	{
		Dialog_AddCheckBox( "bounceEnabled", "Bounce light", g_settings.m_bounceEnabled, 0 );
		Dialog_AddCheckBox( "texLights", "Texture lights (rad / info_texlights)", g_settings.m_texLights, 0 );
		Dialog_AddSpinBox( "bounce", "Bounces", g_settings.m_bounce, 0, 32, 1, 0 );

		Dialog_AddSpinBox( "chop", "Lightmap resolution (chop)", g_settings.m_chop, 4, 64, 4, 0 );

		Dialog_AddSpinBoxFloat( "brightness", "Brightness (-scale)", g_settings.m_lightScale, 0.1f, 8.f, 0.1f, 0 );
		Dialog_AddSpinBoxFloat( "gamma", "Light gamma", g_settings.m_lightGamma, 0.1f, 2.f, 0.1f, 0 );

		// check box instead of a spin box: the editor clamped the intended default of 188
		Dialog_AddCheckBox( "limiter188", "Limiter (hlrad 188)", g_settings.m_limitThreshold > 0.f && g_settings.m_limitThreshold < 255.f, 0 );

		Dialog_AddSpinBoxFloat( "sky", "Sky brightness (-sky)", g_settings.m_skyScale, 0.f, 4.f, 0.1f, 0 );

		Dialog_AddCheckBox( "preview", "Preview in viewport", g_settings.m_preview, 0 );

		Dialog_AddCheckBox( "restoreOnClick", "Restore on first left click", g_settings.m_restoreOnClick, 0 );

		Dialog_AddCheckBox( "renameTextures", "Rename face textures (writes into the map)", g_settings.m_renameTextures, 0 );

		Dialog_AddDirectoryEdit( "wadpath", "WAD directory", g_wadPath, 0 );

		Dialog_InitInternalCommand( "Compile", LB_DoCompile );

		Dialog_Printf( "Press \"Compile\" to bake the lighting of the current map." );
	}
	Dialog_End();
}

// bacontsu: removes generated texture names that ended up in the map. The original name is only
static void LB_ActionRepairNames( int actionUserData )
{
	qWorld_s *world = Global_GetCurrentWorld();

	if ( !world )
	{
		Dialog_MessageBox( "Fix baked texture names", "No map is loaded.", DIALOG_MB_ICONERROR );
		return;
	}

	if ( !g_wadPath[0] )
		Sys_GetModDirectory( g_wadPath, sizeof( g_wadPath ) );

	LB_EnsureTextureCache( g_wadPath );

	int repaired = 0;
	int unrecoverable = 0;

	for ( qEntity_s *entity = world->m_entityList; entity; entity = entity->next )
	{
		for ( qBrush_s *brush = entity->m_brushList; brush; brush = brush->next )
		{
			for ( qFace_s *face = brush->m_faceList; face; face = face->next )
			{
				char recovered[MAX_PATH] = { 0 };

				if ( !LB_ParseBakedName( face->m_texDef.m_textureName, recovered, sizeof( recovered ) ) )
					continue;

				if ( !g_textureCache.Find( recovered ) )
				{
					unrecoverable++;
					continue;
				}

				strncpy( face->m_texDef.m_textureName, recovered, sizeof( face->m_texDef.m_textureName ) - 1 );
				face->m_texDef.m_textureName[sizeof( face->m_texDef.m_textureName ) - 1] = '\0';

				face->m_shaderInfo = Shader_Lookup( recovered );

				repaired++;
			}
		}
	}

	LB_LogMsg( "repair: %d names repaired, %d unrecoverable", repaired, unrecoverable );

	char message[512];

	snprintf( message, sizeof( message ),
		"Repaired %d face(s).\n\n%d face(s) carried a name from an older plugin build that does not contain the "
		"original texture name, so they cannot be recovered automatically. Restore those from a backup of the map.",
		repaired, unrecoverable );

	Dialog_MessageBox( "Fix baked texture names", message, repaired > 0 ? DIALOG_MB_ICONINFORMATION : DIALOG_MB_ICONWARNING );
}

// bacontsu: puts back the original face textures
static void LB_ActionRestore( int actionUserData )
{
	if ( !LB_HasPreview() )
	{
		Dialog_MessageBox( "Restore baked lighting", "Nothing to restore: no baked preview is active.", DIALOG_MB_ICONINFORMATION );
		return;
	}

	if ( LB_RestorePreview() )
		Dialog_MessageBox( "Restore baked lighting", "The original face textures were restored.", DIALOG_MB_ICONINFORMATION );
	else
		Dialog_MessageBox( "Restore baked lighting", "Failed to restore the face textures.", DIALOG_MB_ICONERROR );
}

// clang-format off
static pluginActionDesc_t g_actionCompile =
{
#if JACK_API_VERSION >= API_VERSION_STEAM_BETA
	"CompileLight",
#endif // JACK_API_VERSION >= API_VERSION_STEAM_BETA
	"&Compile Light",
	"Bakes lighting for the current map (hlrad style) and previews it",
	"Half-Life",
#if JACK_API_VERSION <= API_VERSION_STEAM_PUBLIC
	0,
#endif // JACK_API_VERSION <= API_VERSION_STEAM_PUBLIC
	0,
	0,
	LB_ActionCompile
};

static pluginActionDesc_t g_actionRestore =
{
#if JACK_API_VERSION >= API_VERSION_STEAM_BETA
	"RestoreBakedLighting",
#endif // JACK_API_VERSION >= API_VERSION_STEAM_BETA
	"&Restore baked lighting",
	"Restores the face textures changed by the light baker",
	"Half-Life",
#if JACK_API_VERSION <= API_VERSION_STEAM_PUBLIC
	0,
#endif // JACK_API_VERSION <= API_VERSION_STEAM_PUBLIC
	0,
	0,
	LB_ActionRestore
};
// clang-format on

static pluginActionDesc_t g_actionRepair =
{
#if JACK_API_VERSION >= API_VERSION_STEAM_BETA
	"FixBakedTextureNames",
#endif // JACK_API_VERSION >= API_VERSION_STEAM_BETA
	"&Fix baked texture names",
	"Removes light baker texture names that were saved into the map",
	"Half-Life",
#if JACK_API_VERSION <= API_VERSION_STEAM_PUBLIC
	0,
#endif // JACK_API_VERSION <= API_VERSION_STEAM_PUBLIC
	0,
	0,
	LB_ActionRepairNames
};

DLL_EXPORT int vpEnumActions( pfnRegisterAction registerAction, void *pluginManager )
{
	registerAction( &g_actionCompile, pluginManager );
	registerAction( &g_actionRestore, pluginManager );
	registerAction( &g_actionRepair, pluginManager );

	return 3;
}
