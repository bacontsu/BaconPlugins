//=====================================================================================//
// bacontsu: viewport preview - bakes light into face textures
//=====================================================================================//
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <vector>

#include "GL/glew.h"

#include "lightbake.h"

extern CTextureCache g_textureCache;

#define LB_PREVIEW_MAXSIZE		512		// max texture size per face
#define LB_PREVIEW_MINSIZE		4
#define LB_PREVIEW_MEMORY		( 192 * 1024 * 1024 )	// total texture budget

struct lbFaceSnapshot_t
{
	int m_entityId;
	int m_brushId;
	int m_faceId;

	qShader_s *m_originalShader;
	qTexDef_t m_originalTexDef;
	std::vector<vec3_t> m_originalUV;

	qShader_s *m_bakedShader;

	bool m_applied;
};

static std::vector<lbFaceSnapshot_t> s_snapshots;

// world the snapshots belong to, so a restored map can't be written into another one
static qWorld_s *s_previewWorld = NULL;

// incremented on every preview build, keeps generated shader names unique
static int s_bakeSerial = 0;

// shaders kept alive by a keep-shaders restore; freed on the next bake
static std::vector<qShader_s *> s_pendingShaders;

static void LB_FlushPendingShaders()
{
	for ( size_t i = 0; i < s_pendingShaders.size(); i++ )
	{
		if ( s_pendingShaders[i] )
			Shader_Destroy( s_pendingShaders[i] );
	}

	s_pendingShaders.clear();
}

// bacontsu: bilinear, wrapping, palette free sampling of a WAD texture
static void LB_SampleBaseTexture( const lbTexture_t *texture, float u, float v, float &rOut, float &gOut, float &bOut )
{
	if ( !texture || !texture->m_pixels )
	{
		rOut = gOut = bOut = 160.f;
		return;
	}

	int w = texture->m_width;
	int h = texture->m_height;

	u = fmodf( u, (float)w );
	v = fmodf( v, (float)h );

	if ( u < 0.f )
		u += (float)w;
	if ( v < 0.f )
		v += (float)h;

	int x0 = (int)floorf( u );
	int y0 = (int)floorf( v );

	int x1 = ( x0 + 1 ) % w;
	int y1 = ( y0 + 1 ) % h;

	x0 %= w;
	y0 %= h;

	float tx = u - floorf( u );
	float ty = v - floorf( v );

	int i00 = ( y0 * w + x0 ) * 3;
	int i10 = ( y0 * w + x1 ) * 3;
	int i01 = ( y1 * w + x0 ) * 3;
	int i11 = ( y1 * w + x1 ) * 3;

	const unsigned char *p = texture->m_pixels;

	float r0 = p[i00] * ( 1.f - tx ) + p[i10] * tx;
	float r1 = p[i01] * ( 1.f - tx ) + p[i11] * tx;

	float g0 = p[i00 + 1] * ( 1.f - tx ) + p[i10 + 1] * tx;
	float g1 = p[i01 + 1] * ( 1.f - tx ) + p[i11 + 1] * tx;

	float b0 = p[i00 + 2] * ( 1.f - tx ) + p[i10 + 2] * tx;
	float b1 = p[i01 + 2] * ( 1.f - tx ) + p[i11 + 2] * tx;

	rOut = r0 * ( 1.f - ty ) + r1 * ty;
	gOut = g0 * ( 1.f - ty ) + g1 * ty;
	bOut = b0 * ( 1.f - ty ) + b1 * ty;
}

// bacontsu: uploads the baked image and returns a shader that can be assigned to a face
static qShader_s *LB_MakePreviewShader( const char *hashName, unsigned char *pixels, int width, int height )
{
	qShader_s *shaderDef = Shader_Create( hashName, hashName, 0 );

	if ( !shaderDef )
		return NULL;

	if ( shaderDef->m_stage )
		return NULL;

	qShaderStage_t shaderStage;
	memset( &shaderStage, 0, sizeof( shaderStage ) );

	qTexture_t *textureDef = Shader_UploadTexture( shaderDef, hashName, GL_RGB, GL_RGB, 3, width, height, true, pixels );

	if ( !textureDef )
	{
		Shader_Destroy( shaderDef );
		return NULL;
	}

	shaderStage.m_currentTexture = textureDef;

	shaderDef->m_texture = textureDef;

	Shader_AddStage( shaderDef, &shaderStage );
	Shader_Finish( shaderDef );

	return shaderDef;
}

static bool LB_RestorePreviewInternal( bool destroyShaders )
{
	if ( s_snapshots.empty() )
		return false;

	qWorld_s *world = Global_GetCurrentWorld();

	bool canRestore = ( world != NULL && world == s_previewWorld );

	if ( !world )
		return false;

	// find every face we touched by its editor id, so a face rebuild can't dangle
	for ( size_t i = 0; i < s_snapshots.size() && canRestore; i++ )
	{
		lbFaceSnapshot_t &snapshot = s_snapshots[i];

		if ( !snapshot.m_applied )
			continue;

		qFace_s *face = NULL;

		for ( qEntity_s *entity = world->m_entityList; entity && !face; entity = entity->next )
		{
			if ( entity->m_editorId != snapshot.m_entityId )
				continue;

			for ( qBrush_s *brush = entity->m_brushList; brush && !face; brush = brush->next )
			{
				if ( brush->m_editorId != snapshot.m_brushId )
					continue;

				for ( qFace_s *candidate = brush->m_faceList; candidate; candidate = candidate->next )
				{
					if ( candidate->m_editorId == snapshot.m_faceId )
					{
						face = candidate;
						break;
					}
				}
			}
		}

		if ( !face )
			continue;

		face->m_shaderInfo = snapshot.m_originalShader;
		face->m_texDef = snapshot.m_originalTexDef;

		if ( face->m_vertices && snapshot.m_originalUV.size() == (size_t)face->m_vertexCount )
		{
			for ( int v = 0; v < face->m_vertexCount; v++ )
				face->m_vertices[v].uv = snapshot.m_originalUV[v];
		}
	}

	// free the generated shaders (or park them until the next bake, when a GL context
	for ( size_t i = 0; i < s_snapshots.size(); i++ )
	{
		if ( !s_snapshots[i].m_bakedShader )
			continue;

		if ( destroyShaders )
			Shader_Destroy( s_snapshots[i].m_bakedShader );
		else
			s_pendingShaders.push_back( s_snapshots[i].m_bakedShader );
	}

	s_snapshots.clear();
	s_previewWorld = NULL;

	// the click hook is only needed while a preview is applied
	LB_InstallAutoRestoreHook( false );

	return canRestore;
}

bool LB_RestorePreview()
{
	return LB_RestorePreviewInternal( true );
}

bool LB_RestorePreviewKeepShaders()
{
	return LB_RestorePreviewInternal( false );
}

bool LB_HasPreview()
{
	return !s_snapshots.empty();
}

bool LB_BuildPreview( CMapGeometry &geometry, const lbSettings_t &settings, const std::function<void( int, int, const char * )> &progressFn )
{
	if ( LB_HasPreview() )
		LB_RestorePreview();

	LB_FlushPendingShaders();

	s_bakeSerial++;

	s_previewWorld = Global_GetCurrentWorld();

	size_t memoryBudget = LB_PREVIEW_MEMORY;
	size_t memoryUsed = 0;

	int bakedCount = 0;
	int skippedCount = 0;

	int faceCount = (int)geometry.m_faces.size();

	for ( int i = 0; i < faceCount; i++ )
	{
		lbFace_t &face = geometry.m_faces[i];

		if ( !face.m_lit || face.m_luxelLight.empty() || !face.m_face )
			continue;

		if ( progressFn && ( i & 15 ) == 0 )
			progressFn( i, faceCount, "Building viewport preview..." );

		float extentU = face.m_bbMax.x - face.m_bbMin.x;
		float extentV = face.m_bbMax.y - face.m_bbMin.y;

		if ( extentU < 1.f || extentV < 1.f )
		{
			skippedCount++;
			continue;
		}

		// one texel per world unit where possible; only very large faces get coarser texels
		float step = 1.f;

		float largest = std::max( extentU, extentV );

		if ( largest > (float)LB_PREVIEW_MAXSIZE )
			step = ceilf( largest / (float)LB_PREVIEW_MAXSIZE );

		int width = (int)ceilf( extentU / step );
		int height = (int)ceilf( extentV / step );

		width = std::max( width, LB_PREVIEW_MINSIZE );
		height = std::max( height, LB_PREVIEW_MINSIZE );

		// power of two, otherwise this GL build wraps and mipmaps them wrong
		auto roundToPowerOfTwo = []( int value ) -> int
		{
			int result = 1;

			while ( result < value )
				result <<= 1;

			return result;
		};

		width = roundToPowerOfTwo( width );
		height = roundToPowerOfTwo( height );

		if ( width > LB_PREVIEW_MAXSIZE )
			width = LB_PREVIEW_MAXSIZE;
		if ( height > LB_PREVIEW_MAXSIZE )
			height = LB_PREVIEW_MAXSIZE;

		size_t imageBytes = (size_t)width * (size_t)height * 3;

		if ( memoryUsed + imageBytes > memoryBudget )
		{
			Sys_Warning( "LightBake: texture budget exhausted, %d faces were not previewed", faceCount - i );
			skippedCount += faceCount - i;
			break;
		}

		unsigned char *pixels = (unsigned char *)malloc( imageBytes );

		if ( !pixels )
		{
			skippedCount++;
			continue;
		}

		const lbTexture_t *baseTexture = g_textureCache.Find( face.m_texName );

		vec3_t origin = face.m_verts[0];

		for ( int y = 0; y < height; y++ )
		{
			for ( int x = 0; x < width; x++ )
			{
				float fx = face.m_bbMin.x + ( x + 0.5f ) * step;
				float fy = face.m_bbMin.y + ( y + 0.5f ) * step;

				vec3_t pos = origin + face.m_e1 * ( fx - DotProduct( origin, face.m_e1 ) ) + face.m_e2 * ( fy - DotProduct( origin, face.m_e2 ) );

				float dist = DotProduct( face.m_normal, pos ) - face.m_planeDist;
				pos -= face.m_normal * dist;

				float u = face.m_uvGradU.x * fx + face.m_uvGradU.y * fy + face.m_uvOffset.x;
				float v = face.m_uvGradV.x * fx + face.m_uvGradV.y * fy + face.m_uvOffset.y;

				float r, g, b;
				LB_SampleBaseTexture( baseTexture, u, v, r, g, b );

				vec3_t light = face.SampleLight( fx, fy );

				float lr = light.x / 255.f;
				float lg = light.y / 255.f;
				float lb = light.z / 255.f;

				int index = ( y * width + x ) * 3;

				int ir = (int)( r * lr );
				int ig = (int)( g * lg );
				int ib = (int)( b * lb );

				pixels[index + 0] = (unsigned char)std::min( 255, std::max( 0, ir ) );
				pixels[index + 1] = (unsigned char)std::min( 255, std::max( 0, ig ) );
				pixels[index + 2] = (unsigned char)std::min( 255, std::max( 0, ib ) );
			}
		}

		char hashName[64];

		// name keeps the original texture so a map saved by accident stays repairable
		char originalName[32] = { 0 };

		for ( int n = 0; n < 31 && face.m_texName[n]; n++ )
		{
			char c = face.m_texName[n];

			bool safe = ( c >= 'A' && c <= 'Z' ) || ( c >= 'a' && c <= 'z' ) || ( c >= '0' && c <= '9' ) || c == '_';

			originalName[n] = safe ? c : '_';
		}

		snprintf( hashName, sizeof( hashName ), "lb%d_%d_%s", s_bakeSerial, face.m_faceId, originalName );

		qShader_s *bakedShader = LB_MakePreviewShader( hashName, pixels, width, height );

		free( pixels );

		if ( !bakedShader )
		{
			skippedCount++;
			continue;
		}

		memoryUsed += imageBytes;
		bakedCount++;

		// snapshot the face so we can put it back later
		lbFaceSnapshot_t snapshot;
		memset( &snapshot, 0, sizeof( snapshot ) );

		snapshot.m_entityId = face.m_entityId;
		snapshot.m_brushId = face.m_brushId;
		snapshot.m_faceId = face.m_faceId;

		snapshot.m_originalShader = face.m_face->m_shaderInfo;
		snapshot.m_originalTexDef = face.m_face->m_texDef;
		snapshot.m_originalUV.resize( face.m_face->m_vertexCount );

		for ( int v = 0; v < face.m_face->m_vertexCount; v++ )
			snapshot.m_originalUV[v] = face.m_face->m_vertices[v].uv;

		snapshot.m_bakedShader = bakedShader;
		snapshot.m_applied = true;

		s_snapshots.push_back( snapshot );

		if ( settings.m_overrideUVs )
		{
			// uv only, texdef stays untouched so a save can't pick up our texture names
			if ( face.m_face->m_vertices )
			{
				for ( int v = 0; v < face.m_face->m_vertexCount; v++ )
				{
					const vec3_t &pos = face.m_face->m_vertices[v].coords;

					float x = DotProduct( pos, face.m_e1 );
					float y = DotProduct( pos, face.m_e2 );

					float u = ( x - face.m_bbMin.x ) / ( step * (float)width );
					float t = ( y - face.m_bbMin.y ) / ( step * (float)height );

					face.m_face->m_vertices[v].uv = vec3_t( u, t, 0.f );
				}
			}

			// only needed if the editor resolves shaders by name, it writes our name in the map
			if ( settings.m_renameTextures )
			{
				qTexDef_t texDef = face.m_face->m_texDef;

				texDef.m_UAxis = vec4_t( face.m_e1.x, face.m_e1.y, face.m_e1.z, -face.m_bbMin.x / step );
				texDef.m_VAxis = vec4_t( face.m_e2.x, face.m_e2.y, face.m_e2.z, -face.m_bbMin.y / step );
				texDef.m_scale = vec2_t( step, step );
				texDef.m_rotate = 0.f;
				texDef.m_textureAlignment = TEXALIGN_FACE;

				strncpy( texDef.m_textureName, hashName, sizeof( texDef.m_textureName ) - 1 );
				texDef.m_textureName[sizeof( texDef.m_textureName ) - 1] = '\0';

				face.m_face->m_texDef = texDef;
			}
		}

		face.m_face->m_shaderInfo = bakedShader;

		// let the renderer know that this brush changed
		if ( face.m_brush )
			face.m_brush->render_unkint = 0;
	}

	if ( progressFn )
		progressFn( faceCount, faceCount, "Building viewport preview..." );

	if ( settings.m_verbose )
	{
		Sys_Printf( "LightBake: %d faces previewed, %d skipped, %d KiB of textures", bakedCount, skippedCount, (int)( memoryUsed / 1024 ) );
	}

	// a left click means the user wants to edit, so get the original textures out of the way
	if ( bakedCount > 0 && settings.m_restoreOnClick )
		LB_InstallAutoRestoreHook( true );

	return bakedCount > 0;
}
