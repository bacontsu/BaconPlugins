//=====================================================================================//
// bacontsu: shared types
//=====================================================================================//
#if !defined( LIGHTBAKE_H )
#define LIGHTBAKE_H

#include <vector>
#include <string>
#include <functional>

#include "PluginMeta.h"

#include "wad.h"

// light falloff modes (mirrors hlrad's _falloff keyvalue)
#define LB_FALLOFF_LINEAR				0
#define LB_FALLOFF_LINEAR_SCALE			1
#define LB_FALLOFF_INVERSE				2
#define LB_FALLOFF_INVERSE_SQUARE		3

// hlrad constants
#define LB_MINLIGHT						( 188.0f / 255.0f )	// hlrad MINLIGHT, in 0..1 space
#define LB_LIGHT_EPSILON				0.05f
#define LB_MAX_LIGHT_VALUE				255.0f

struct lbVec2
{
	float x, y;

	lbVec2() { x = y = 0.f; }
	lbVec2( float x, float y ) { this->x = x; this->y = y; }
};

// bacontsu: a single light source gathered from the map (light / light_spot / light_environment)
struct lbLight_t
{
	vec3_t m_origin;
	vec3_t m_color;			// r, g, b (0..255 scale, already multiplied by brightness)
	vec3_t m_direction;		// normalized emission direction (spot lights only)
	float m_radius;			// _distance, 0 = infinite
	float m_stopdot;		// cos( _cone )
	float m_stopdot2;		// cos( _cone2 )
	bool m_spot;
	int m_falloff;
};

// bacontsu: sky lighting settings (light_environment)
struct lbSky_t
{
	bool m_enabled;
	vec3_t m_color;			// _light (sun) * scaler, 0..255 scale, NOT squared
	vec3_t m_diffuse;		// _diffuse_light (sky dome), falls back to m_color
	vec3_t m_direction;		// normalized direction the sun points TO (downwards)
};

// bacontsu: a face of the map, with its lightmap grid
struct lbFace_t
{
	qFace_t *m_face;
	qBrush_s *m_brush;
	qEntity_s *m_entity;

	int m_faceId;
	int m_brushId;
	int m_entityId;

	vec3_t m_normal;
	float m_planeDist;

	// in-plane orthonormal basis: world position = m_origin + e1 * x + e2 * y
	vec3_t m_e1;
	vec3_t m_e2;
	lbVec2 m_bbMin;			// extents in (e1,e2) space
	lbVec2 m_bbMax;

	// winding in world space
	std::vector<vec3_t> m_verts;

	// texture uv of every vertex (texels), fitted affinely over (e1,e2)
	std::vector<lbVec2> m_uv;

	// affine texture mapping: uv = m_uvGrad * (x,y) + m_uvOffset
	lbVec2 m_uvGradU;
	lbVec2 m_uvGradV;
	lbVec2 m_uvOffset;

	char m_texName[64];
	int m_texWidth;
	int m_texHeight;

	bool m_occluder;		// blocks light
	bool m_sky;				// sky surface (light_environment applies)
	bool m_lit;				// receives a lightmap
	bool m_liquid;

	vec3_t m_albedo;		// average texture color (0..1), used by the bounce pass

	bool m_texLight;
	vec3_t m_texLightColor;

	// lightmap grid, in (e1,e2) space, spacing = m_chop
	float m_chop;
	int m_lmW;
	int m_lmH;
	lbVec2 m_lmMin;

	std::vector<vec3_t> m_luxelPos;		// lmW*lmH world positions
	std::vector<vec3_t> m_luxelDirect;	// direct + sky light (0..255 per channel)
	std::vector<vec3_t> m_luxelLight;	// final light (direct + indirect)

	lbFace_t();
	void Clear();

	// bilinearly samples the lightmap at (x,y) in (e1,e2) space
	vec3_t SampleLight( float x, float y ) const;
};

// bacontsu: surface emitter built from a texture light
struct lbEmitter_t
{
	vec3_t m_origin;
	vec3_t m_normal;
	vec3_t m_power;			// color * albedo / pi, area is kept separate for the spot fix
	float m_area;
	float m_stopdot;		// cos( _cone ), 0 for omnidirectional
	float m_stopdot2;
	float m_range;			// 0 = unlimited
	float m_reach;			// squared distance past which this emitter is invisible
};

// bacontsu: light_surface entity
struct lbSurfaceLight_t
{
	char m_texture[64];
	char m_classFilter[64];
	char m_nameFilter[64];
	vec3_t m_color;
	vec3_t m_texColor;
	float m_scale;
	float m_stopdot;
	float m_stopdot2;
	float m_range;
	bool m_hasTexColor;
};

struct lbTri_t
{
	vec3_t m_v0;
	vec3_t m_v1;
	vec3_t m_v2;
	vec3_t m_normal;
	float m_planeDist;
	int m_faceIndex;
};

class CBVH
{
public:
	CBVH();

	void Build( const std::vector<lbTri_t> &tris );
	void Purge();

	// returns true when something blocks the segment [start, start + dir * maxDist]
	bool IsBlocked( const vec3_t &start, const vec3_t &dir, float maxDist ) const;

	// returns the hit distance, or -1 when nothing was hit (triIndexOut gets the triangle index)
	float Trace( const vec3_t &start, const vec3_t &dir, float maxDist, int *triIndexOut = NULL ) const;

	// face index that owns the given triangle
	int GetTriangleFace( int triIndex ) const;

	bool IsEmpty() const { return m_indices.empty(); }

	int GetTriCount() const { return (int)m_indices.size(); }

private:
	struct Node
	{
		vec3_t m_bbMin;
		vec3_t m_bbMax;
		int m_left;
		int m_right;
		int m_first;
		int m_count;
	};

	int BuildNode( int first, int count, int depth );

	std::vector<lbTri_t> m_storage;
	const lbTri_t *m_tris;
	std::vector<int> m_indices;
	std::vector<Node> m_nodes;
};

class CMapGeometry
{
public:
	CMapGeometry();
	~CMapGeometry();

	void Purge();

	// collects every face / light of the world
	bool Build( qWorld_s *world );

	// recomputes the lightmap grid of every face for the given sample spacing
	void BuildLightmaps( int chop );

	// bacontsu: builds surface emitters from the texture lights of every face
	void BuildEmitters( int texChop );

	std::vector<lbFace_t> m_faces;
	std::vector<lbLight_t> m_lights;
	std::vector<lbEmitter_t> m_emitters;
	std::vector<lbSurfaceLight_t> m_surfaceLights;
	lbSky_t m_sky;

	CBVH m_bvh;

	int m_numLitFaces;
	int m_numLuxels;
	int m_numOccluders;
	int m_numSkippedLights;
	int m_numTexLightFaces;

private:
	void AddFace( qEntity_s *entity, qBrush_s *brush, qFace_s *face );
	void AddLight( qEntity_s *entity );
	void AddSurfaceLight( qEntity_s *entity );
};

// bacontsu: light settings (mirrors the more useful hlrad options)
struct lbSettings_t
{
	int m_chop;					// -chop (default 16)
	int m_bounce;				// -bounce (default 8)
	bool m_bounceEnabled;
	float m_bounceColorScale;	// -bouncecolorscale (default 0.0 = colorless bounce)
	float m_lightScale;			// -scale (default 2.0)
	float m_skyScale;			// -sky (sky brightness, default 1.0)
	float m_lightGamma;			// -gamma (default 0.5)
	float m_limitThreshold;		// -limiter (default 188, <= 0 disables)
	float m_minLight;			// -minlight (default 0)
	bool m_bounceOnlySky;		// never bounce from a sky face
	int m_numThreads;
	bool m_texLights;			// lights.rad / info_texlights / light_surface
	int m_texChop;				// -texchop, emitter patch size
	bool m_preview;				// swap textures in the viewport
	bool m_restoreOnClick;		// restore the preview on the first left click
	bool m_overrideUVs;
	bool m_renameTextures;
	bool m_verbose;
};

void LB_DefaultSettings( lbSettings_t &settingsOut );

// appends to %TEMP%\vpLightBake.log, never touches the editor API so it survives a crash
void LB_LogMsg( const char *format, ... );

// bacontsu: recognises a face texture name generated by this plugin ("lb<bake>_<face>_<original>")
bool LB_ParseBakedName( const char *name, char *originalOut, size_t originalSize );

// texture lights, filled from *.rad files and info_texlights entities
void LB_ClearTexLights();
int LB_LoadRadFile( const char *filePath );
int LB_LoadRadDirectory( const char *dir );
int LB_CollectInfoTexLights( qWorld_s *world );
bool LB_FindTexLight( const char *textureName, vec3_t &colorOut, bool &fromTextureOut );
int LB_GetTexLightCount();

class CRad
{
public:
	CRad( CMapGeometry &geometry, const lbSettings_t &settings );

	// runs the whole compile. progressFn(current, total) may be NULL
	bool Run( const std::function<void( int, int, const char * )> &progressFn );

	// stats printed at the end
	float m_directTime;
	float m_bounceTime;
	int m_numPatches;

private:
	void ComputeDirect();
	void ComputeBounce();
	void FinishLightmaps();

	CMapGeometry &m_geometry;
	lbSettings_t m_settings;

	std::function<void( int, int, const char * )> m_progressFn;
};

// bacontsu: viewport preview: bakes the lightmap into per-face textures and swaps them in
bool LB_BuildPreview( CMapGeometry &geometry, const lbSettings_t &settings, const std::function<void( int, int, const char * )> &progressFn );

// restores every face touched by LB_BuildPreview
bool LB_RestorePreview();

// same, but keeps the generated shaders for the next bake (no GL context in a mouse hook)
bool LB_RestorePreviewKeepShaders();

// installs/uninstalls the thread local mouse hook that restores on a left click
void LB_InstallAutoRestoreHook( bool install );

bool LB_HasPreview();

#endif // !LIGHTBAKE_H
