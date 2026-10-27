//=====================================================================================//
// bacontsu: gathers faces, lights and occluders from the map
//=====================================================================================//
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <algorithm>
#include <vector>

#include "lightbake.h"

extern CTextureCache g_textureCache;

#if !defined( WIN32 )
#define strnicmp strncasecmp
#endif

static const char *LB_GetKeyValue( qEntity_s *entity, const char *key );

// bacontsu: rendermode may be a number or a name depending on who wrote the map
static int LB_ParseRenderMode( const char *value )
{
	if ( !value || !value[0] )
		return 0;

	if ( isdigit( (unsigned char)value[0] ) || value[0] == '-' )
		return atoi( value );

	if ( !stricmp( value, "normal" ) )
		return 0;
	if ( !stricmp( value, "color" ) )
		return 1;
	if ( !stricmp( value, "texture" ) )
		return 2;
	if ( !stricmp( value, "glow" ) )
		return 3;
	if ( !stricmp( value, "solid" ) )
		return 4;
	if ( !stricmp( value, "additive" ) )
		return 5;

	return 0;
}

// bacontsu: brushes that should not cast shadows, worldspawn always does
static bool LB_EntityDoesNotBlockLight( qEntity_s *entity )
{
	if ( !entity )
		return false;

	const char *className = entity->m_className;

	if ( !className || !className[0] || !stricmp( className, "worldspawn" ) )
		return false;

	static const char *s_nonSolid[] =
	{
		"func_illusionary", "func_water", "func_ladder", "func_monsterclip",
		"func_buyzone", "func_vehicleclip", "func_grenadeclip", "func_friction",
		NULL
	};

	for ( int i = 0; s_nonSolid[i]; i++ )
	{
		if ( !stricmp( className, s_nonSolid[i] ) )
			return true;
	}

	if ( !strnicmp( className, "trigger_", 8 ) )
		return true;

	const char *lightFlags = LB_GetKeyValue( entity, "zhlt_lightflags" );

	if ( lightFlags && ( atoi( lightFlags ) & 8 ) )
		return true;

	// the editor keeps the render mode in the entity, the keyvalue is only there when the
	// map was made by something that writes it out
	int renderMode = entity->m_renderMode.m_renderMode;

	const char *renderModeKey = LB_GetKeyValue( entity, "rendermode" );

	if ( renderModeKey )
	{
		int parsed = LB_ParseRenderMode( renderModeKey );

		if ( parsed != 0 )
			renderMode = parsed;
	}

	// glow and additive always let light through, the others only blend below full alpha
	if ( renderMode == 3 || renderMode == 5 )
		return true;

	int renderAmount = entity->m_renderMode.m_renderColor.a;

	const char *renderAmtKey = LB_GetKeyValue( entity, "renderamt" );

	if ( renderAmtKey )
	{
		int parsed = atoi( renderAmtKey );

		if ( parsed > 0 )
			renderAmount = parsed;
	}

	if ( renderAmount > 0 && renderAmount < 255 )
		return true;

	return false;
}

// bacontsu: "lb<bake>_<face>_<original>" -> "<original>"
bool LB_ParseBakedName( const char *name, char *originalOut, size_t originalSize )
{
	if ( !name || !originalOut || originalSize < 2 )
		return false;

	if ( name[0] != 'l' || name[1] != 'b' )
		return false;

	const char *cursor = name + 2;

	if ( !isdigit( (unsigned char)*cursor ) )
		return false;

	while ( isdigit( (unsigned char)*cursor ) )
		cursor++;

	if ( *cursor != '_' )
		return false;

	cursor++;

	if ( !isdigit( (unsigned char)*cursor ) )
		return false;

	while ( isdigit( (unsigned char)*cursor ) )
		cursor++;

	if ( *cursor != '_' )
		return false;

	cursor++;

	if ( !cursor[0] )
		return false;

	strncpy( originalOut, cursor, originalSize - 1 );
	originalOut[originalSize - 1] = '\0';

	return true;
}

static bool LB_IsUtilityTexture( const char *name )
{
	if ( !name || !name[0] )
		return true;

	if ( name[0] == '+' )
		return true;

	static const char *s_utility[] =
	{
		"NULL", "CLIP", "HINT", "SKIP", "TRIGGER", "ORIGIN", "BOUNDS", "AAATRIGGER",
		"SOLIDHINT", "BEVEL", "SPLITFACE", "HIDDEN", "CONTENTWATER",
		NULL
	};

	for ( int i = 0; s_utility[i]; i++ )
	{
		if ( !stricmp( name, s_utility[i] ) )
			return true;
	}

	return false;
}

static bool LB_IsLiquidTexture( const char *name )
{
	if ( !name || !name[0] )
		return false;

	if ( name[0] == '!' && ( !stricmp( name, "!water" ) || !stricmp( name, "!lava" ) || !stricmp( name, "!slime" ) ) )
		return true;

	return !stricmp( name, "water" ) || !stricmp( name, "lava" ) || !stricmp( name, "slime" );
}

static bool LB_IsSkyTexture( const char *name )
{
	if ( !name || !name[0] )
		return false;

	return !strnicmp( name, "sky", 3 ) || !strnicmp( name, "t_sky", 5 );
}

static const char *LB_GetKeyValue( qEntity_s *entity, const char *key )
{
	for ( epair_s *pair = entity->epairs; pair; pair = pair->next )
	{
		if ( pair->key && pair->value && !stricmp( pair->key, key ) )
			return pair->value;
	}

	return NULL;
}

// bacontsu: parses hlrad "color255" values: "r g b brightness" or a single brightness value
static bool LB_ParseLightValue( const char *value, vec3_t &colorOut, float &brightnessOut )
{
	colorOut = vec3_t( 255.f, 255.f, 255.f );
	brightnessOut = 200.f;

	if ( !value || !value[0] )
		return false;

	float r = 255.f, g = 255.f, b = 255.f, brightness = 0.f;

	int parsed = sscanf( value, "%f %f %f %f", &r, &g, &b, &brightness );

	if ( parsed >= 4 )
	{
		colorOut = vec3_t( r, g, b );
		brightnessOut = brightness;
	}
	else if ( parsed == 1 )
	{
		brightnessOut = r;
	}
	else
	{
		return false;
	}

	return true;
}

// bacontsu: quake/Half-Life angle vectors
static void LB_AngleVectors( const vec3_t &angles, vec3_t &forward )
{
	float pitch = angles[PITCH] * (float)( M_PI * 2.0 / 360.0 );
	float yaw = angles[YAW] * (float)( M_PI * 2.0 / 360.0 );

	float sp = sinf( pitch );
	float cp = cosf( pitch );
	float sy = sinf( yaw );
	float cy = cosf( yaw );

	// quake forward: -sp on Z. Half-Life's light_spot uses pitch -90 = straight down,
	forward = vec3_t( cp * cy, cp * sy, sp );
}

static void LB_MakeBasis( const vec3_t &normal, vec3_t &e1, vec3_t &e2 )
{
	vec3_t up( 0.f, 0.f, 1.f );

	if ( fabsf( normal.z ) > 0.999f )
		up = vec3_t( 1.f, 0.f, 0.f );

	vec3_t t1;
	CrossProduct( up, normal, t1 );
	VectorNormalize( t1 );

	vec3_t t2;
	CrossProduct( normal, t1, t2 );
	VectorNormalize( t2 );

	e1 = t1;
	e2 = t2;
}

static float LB_DistToPlane( const vec3_t &normal, float dist, const vec3_t &point )
{
	return DotProduct( normal, point ) - dist;
}

static vec3_t LB_SampleGrid( const std::vector<vec3_t> &grid, int w, int h, float fx, float fy )
{
	if ( w <= 0 || h <= 0 || grid.empty() )
		return vec3_t( 0.f, 0.f, 0.f );

	if ( w == 1 )
		fx = 0.f;
	if ( h == 1 )
		fy = 0.f;

	if ( fx < 0.f )
		fx = 0.f;
	if ( fy < 0.f )
		fy = 0.f;

	if ( fx > w - 1 )
		fx = (float)( w - 1 );
	if ( fy > h - 1 )
		fy = (float)( h - 1 );

	int x0 = (int)floorf( fx );
	int y0 = (int)floorf( fy );

	int x1 = x0 + 1;
	int y1 = y0 + 1;

	if ( x1 > w - 1 )
		x1 = w - 1;
	if ( y1 > h - 1 )
		y1 = h - 1;

	float tx = fx - x0;
	float ty = fy - y0;

	vec3_t v00 = grid[y0 * w + x0];
	vec3_t v10 = grid[y0 * w + x1];
	vec3_t v01 = grid[y1 * w + x0];
	vec3_t v11 = grid[y1 * w + x1];

	vec3_t top = v00 * ( 1.f - tx ) + v10 * tx;
	vec3_t bottom = v01 * ( 1.f - tx ) + v11 * tx;

	return top * ( 1.f - ty ) + bottom * ty;
}

lbFace_t::lbFace_t()
{
	Clear();
}

void lbFace_t::Clear()
{
	m_face = NULL;
	m_brush = NULL;
	m_entity = NULL;

	m_faceId = 0;
	m_brushId = 0;
	m_entityId = 0;

	m_normal = vec3_t( 0.f, 0.f, 1.f );
	m_planeDist = 0.f;

	m_e1 = vec3_t( 1.f, 0.f, 0.f );
	m_e2 = vec3_t( 0.f, 1.f, 0.f );

	m_bbMin = lbVec2( 0.f, 0.f );
	m_bbMax = lbVec2( 0.f, 0.f );

	m_verts.clear();
	m_uv.clear();

	m_uvGradU = lbVec2( 1.f, 0.f );
	m_uvGradV = lbVec2( 0.f, 1.f );
	m_uvOffset = lbVec2( 0.f, 0.f );

	m_texName[0] = '\0';
	m_texWidth = 0;
	m_texHeight = 0;

	m_occluder = false;
	m_sky = false;
	m_lit = false;
	m_liquid = false;

	m_albedo = vec3_t( 0.5f, 0.5f, 0.5f );

	m_chop = 16.f;
	m_lmW = 0;
	m_lmH = 0;
	m_lmMin = lbVec2( 0.f, 0.f );

	m_luxelPos.clear();
	m_luxelDirect.clear();
	m_luxelLight.clear();
}

vec3_t lbFace_t::SampleLight( float x, float y ) const
{
	if ( m_lmW <= 0 || m_lmH <= 0 || m_luxelLight.empty() )
		return vec3_t( 0.f, 0.f, 0.f );

	float fx = ( x - m_lmMin.x ) / m_chop;
	float fy = ( y - m_lmMin.y ) / m_chop;

	return LB_SampleGrid( m_luxelLight, m_lmW, m_lmH, fx, fy );
}

CBVH::CBVH()
{
	m_tris = NULL;
}

void CBVH::Purge()
{
	m_indices.clear();
	m_nodes.clear();
	m_storage.clear();
	m_tris = NULL;
}

void CBVH::Build( const std::vector<lbTri_t> &tris )
{
	Purge();

	m_storage = tris;
	m_tris = m_storage.empty() ? NULL : &m_storage[0];

	int count = (int)m_storage.size();

	if ( count == 0 )
		return;

	m_indices.resize( count );
	for ( int i = 0; i < count; i++ )
		m_indices[i] = i;

	m_nodes.reserve( count * 2 );
	BuildNode( 0, count, 0 );
}

static void LB_TriBounds( const lbTri_t &tri, vec3_t &bbMin, vec3_t &bbMax )
{
	for ( int i = 0; i < 3; i++ )
	{
		bbMin[i] = std::min( tri.m_v0[i], std::min( tri.m_v1[i], tri.m_v2[i] ) );
		bbMax[i] = std::max( tri.m_v0[i], std::max( tri.m_v1[i], tri.m_v2[i] ) );
	}
}

int CBVH::BuildNode( int first, int count, int depth )
{
	int nodeIndex = (int)m_nodes.size();
	m_nodes.push_back( Node() );

	Node &node = m_nodes[nodeIndex];

	memset( &node, 0, sizeof( Node ) );

	vec3_t bbMin( 1e30f, 1e30f, 1e30f );
	vec3_t bbMax( -1e30f, -1e30f, -1e30f );

	vec3_t centroidMin( 1e30f, 1e30f, 1e30f );
	vec3_t centroidMax( -1e30f, -1e30f, -1e30f );

	for ( int i = 0; i < count; i++ )
	{
		const lbTri_t &tri = m_storage[m_indices[first + i]];

		vec3_t tmin, tmax;
		LB_TriBounds( tri, tmin, tmax );

		for ( int c = 0; c < 3; c++ )
		{
			bbMin[c] = std::min( bbMin[c], tmin[c] );
			bbMax[c] = std::max( bbMax[c], tmax[c] );

			float centroid = ( tmin[c] + tmax[c] ) * 0.5f;
			centroidMin[c] = std::min( centroidMin[c], centroid );
			centroidMax[c] = std::max( centroidMax[c], centroid );
		}
	}

	node.m_bbMin = bbMin;
	node.m_bbMax = bbMax;

	const int kMaxLeafSize = 8;

	if ( count <= kMaxLeafSize || depth > 48 )
	{
		node.m_left = -1;
		node.m_right = -1;
		node.m_first = first;
		node.m_count = count;
		return nodeIndex;
	}

	int axis = 0;
	float bestExtent = 0.f;

	for ( int c = 0; c < 3; c++ )
	{
		float extent = centroidMax[c] - centroidMin[c];

		if ( extent > bestExtent )
		{
			bestExtent = extent;
			axis = c;
		}
	}

	if ( bestExtent < 1e-6f )
	{
		node.m_left = -1;
		node.m_right = -1;
		node.m_first = first;
		node.m_count = count;
		return nodeIndex;
	}

	int half = count / 2;

	std::vector<int> &indices = m_indices;

	std::nth_element( indices.begin() + first, indices.begin() + first + half, indices.begin() + first + count,
		[&]( int a, int b )
		{
			const lbTri_t &ta = m_storage[a];
			const lbTri_t &tb = m_storage[b];

			vec3_t amin, amax, bmin, bmax;
			LB_TriBounds( ta, amin, amax );
			LB_TriBounds( tb, bmin, bmax );

			return ( amin[axis] + amax[axis] ) < ( bmin[axis] + bmax[axis] );
		} );

	int left = BuildNode( first, half, depth + 1 );
	int right = BuildNode( first + half, count - half, depth + 1 );

	// both children may have reallocated the node array
	m_nodes[nodeIndex].m_left = left;
	m_nodes[nodeIndex].m_right = right;
	m_nodes[nodeIndex].m_first = 0;
	m_nodes[nodeIndex].m_count = 0;

	return nodeIndex;
}

static bool LB_RayBox( const vec3_t &start, const vec3_t &invDir, const vec3_t &bbMin, const vec3_t &bbMax, float maxDist, float &tMin )
{
	float t0 = 0.f;
	float t1 = maxDist;

	for ( int i = 0; i < 3; i++ )
	{
		float tNear = ( bbMin[i] - start[i] ) * invDir[i];
		float tFar = ( bbMax[i] - start[i] ) * invDir[i];

		if ( tNear > tFar )
			std::swap( tNear, tFar );

		if ( tNear > t0 )
			t0 = tNear;
		if ( tFar < t1 )
			t1 = tFar;

		if ( t0 > t1 )
			return false;
	}

	tMin = t0;
	return true;
}

static bool LB_RayTri( const lbTri_t &tri, const vec3_t &start, const vec3_t &dir, float maxDist, float &hitDist )
{
	const float kEpsilon = 1e-7f;

	vec3_t edge1 = tri.m_v1 - tri.m_v0;
	vec3_t edge2 = tri.m_v2 - tri.m_v0;

	vec3_t h;
	CrossProduct( dir, edge2, h );

	float det = DotProduct( edge1, h );

	if ( det > -kEpsilon && det < kEpsilon )
		return false;

	float invDet = 1.f / det;

	vec3_t s = start - tri.m_v0;

	float u = DotProduct( s, h ) * invDet;

	if ( u < 0.f || u > 1.f )
		return false;

	vec3_t q;
	CrossProduct( s, edge1, q );

	float v = DotProduct( dir, q ) * invDet;

	if ( v < 0.f || u + v > 1.f )
		return false;

	float t = DotProduct( edge2, q ) * invDet;

	if ( t < 0.01f || t > maxDist )
		return false;

	hitDist = t;
	return true;
}

float CBVH::Trace( const vec3_t &start, const vec3_t &dir, float maxDist, int *triIndexOut ) const
{
	if ( triIndexOut )
		*triIndexOut = -1;

	if ( m_nodes.empty() || !m_tris )
		return -1.f;

	if ( maxDist <= 0.f )
		maxDist = 1e30f;

	vec3_t invDir;
	for ( int i = 0; i < 3; i++ )
	{
		float d = dir[i];
		invDir[i] = ( fabsf( d ) < 1e-9f ) ? 1e30f : 1.f / d;
	}

	struct StackEntry
	{
		int m_node;
		float m_dist;
	};

	StackEntry stack[128];
	int stackSize = 0;

	stack[stackSize].m_node = 0;
	stack[stackSize].m_dist = 0.f;
	stackSize++;

	float best = -1.f;
	int bestTri = -1;

	while ( stackSize > 0 )
	{
		stackSize--;

		int nodeIndex = stack[stackSize].m_node;

		const Node &node = m_nodes[nodeIndex];

		float boxDist;
		float limit = ( best > 0.f ) ? best : maxDist;

		if ( !LB_RayBox( start, invDir, node.m_bbMin, node.m_bbMax, limit, boxDist ) )
			continue;

		if ( node.m_count > 0 )
		{
			for ( int i = 0; i < node.m_count; i++ )
			{
				int triIndex = m_indices[node.m_first + i];

				float hitDist;

				if ( LB_RayTri( m_storage[triIndex], start, dir, ( best > 0.f ) ? best : maxDist, hitDist ) )
				{
					best = hitDist;
					bestTri = triIndex;
				}
			}

			continue;
		}

		if ( stackSize + 2 >= (int)( sizeof( stack ) / sizeof( stack[0] ) ) )
			continue;

		if ( node.m_left >= 0 )
		{
			stack[stackSize].m_node = node.m_left;
			stack[stackSize].m_dist = 0.f;
			stackSize++;
		}

		if ( node.m_right >= 0 )
		{
			stack[stackSize].m_node = node.m_right;
			stack[stackSize].m_dist = 0.f;
			stackSize++;
		}
	}

	return best;
}

int CBVH::GetTriangleFace( int triIndex ) const
{
	if ( triIndex < 0 || triIndex >= (int)m_storage.size() )
		return -1;

	return m_storage[triIndex].m_faceIndex;
}

bool CBVH::IsBlocked( const vec3_t &start, const vec3_t &dir, float maxDist ) const
{
	return Trace( start, dir, maxDist ) > 0.f;
}

CMapGeometry::CMapGeometry()
{
	m_numLitFaces = 0;
	m_numLuxels = 0;
	m_numOccluders = 0;
	m_numSkippedLights = 0;
	m_numTexLightFaces = 0;
}

CMapGeometry::~CMapGeometry()
{
	Purge();
}

void CMapGeometry::Purge()
{
	m_faces.clear();
	m_lights.clear();
	m_emitters.clear();
	m_surfaceLights.clear();
	m_bvh.Purge();

	m_sky.m_enabled = false;
	m_sky.m_color = vec3_t( 0.f, 0.f, 0.f );
	m_sky.m_direction = vec3_t( 0.f, 0.f, 1.f );

	m_numLitFaces = 0;
	m_numLuxels = 0;
	m_numOccluders = 0;
	m_numSkippedLights = 0;
}

void CMapGeometry::AddLight( qEntity_s *entity )
{
	const char *className = entity->m_className;

	if ( !className )
		return;

	bool isSpot = !stricmp( className, "light_spot" );
	bool isEnv = !stricmp( className, "light_environment" );

	if ( !isSpot && !isEnv && stricmp( className, "light" ) )
		return;

	const char *originStr = LB_GetKeyValue( entity, "origin" );
	if ( !originStr )
		originStr = LB_GetKeyValue( entity, "_origin" );

	vec3_t origin = entity->m_vecOrigin;

	if ( originStr )
		sscanf( originStr, "%f %f %f", &origin.x, &origin.y, &origin.z );

	const char *lightStr = LB_GetKeyValue( entity, "_light" );

	vec3_t color;
	float brightness;

	LB_ParseLightValue( lightStr, color, brightness );

	if ( brightness <= 0.f || ( color.x <= 0.f && color.y <= 0.f && color.z <= 0.f ) )
		return;

	vec3_t angles = entity->m_vecAngles;

	const char *angleStr = LB_GetKeyValue( entity, "angles" );
	if ( !angleStr )
		angleStr = LB_GetKeyValue( entity, "angle" );

	if ( angleStr )
	{
		float yaw = 0.f;

		if ( sscanf( angleStr, "%f %f %f", &angles.x, &angles.y, &angles.z ) == 1 )
		{
			yaw = angles.x;
			angles = vec3_t( 0.f, yaw, 0.f );
		}
	}

	const char *pitchStr = LB_GetKeyValue( entity, "pitch" );
	if ( pitchStr )
		angles.x = (float)atof( pitchStr );

	if ( isEnv )
	{
		// sky lighting: _light is the directional sun and is NOT squared like point lights
		vec3_t forward;
		LB_AngleVectors( angles, forward );
		VectorNormalize( forward );

		m_sky.m_enabled = true;
		m_sky.m_color = color * ( brightness / 255.f );
		m_sky.m_direction = forward;

		vec3_t diffuse;
		float diffuseBrightness;

		const char *diffuseStr = LB_GetKeyValue( entity, "_diffuse_light" );

		if ( LB_ParseLightValue( diffuseStr, diffuse, diffuseBrightness ) )
			m_sky.m_diffuse = diffuse * ( diffuseBrightness / 255.f );
		else
			m_sky.m_diffuse = m_sky.m_color;

		return;
	}

	lbLight_t light;
	memset( &light, 0, sizeof( light ) );

	light.m_origin = origin;

	// hlrad squares the max channel, divides it by 10 and multiplies the color by that,
	// odd but it is what makes a 255 light read as 255 at ~80 units
	vec3_t intensity = color * ( brightness / 255.f );

	float l1 = std::max( intensity.x, std::max( intensity.y, intensity.z ) );
	l1 = l1 * l1 / 10.f;

	if ( l1 > 0.f )
		intensity = intensity * l1;

	light.m_color = intensity;
	light.m_radius = 0.f;
	light.m_spot = isSpot;
	light.m_stopdot = 1.f;
	light.m_stopdot2 = 1.f;
	light.m_falloff = LB_FALLOFF_INVERSE_SQUARE;

	const char *distanceStr = LB_GetKeyValue( entity, "_distance" );
	if ( distanceStr )
		light.m_radius = (float)atof( distanceStr );

	const char *falloffStr = LB_GetKeyValue( entity, "_falloff" );
	if ( falloffStr )
		light.m_falloff = atoi( falloffStr );

	if ( isSpot )
	{
		float cone = 30.f;
		float cone2 = 45.f;

		const char *coneStr = LB_GetKeyValue( entity, "_cone" );
		if ( coneStr )
			cone = (float)atof( coneStr );

		const char *cone2Str = LB_GetKeyValue( entity, "_cone2" );
		if ( cone2Str )
			cone2 = (float)atof( cone2Str );

		if ( cone2 < cone )
			std::swap( cone, cone2 );

		// a light with _sky set is a sky light, it only affects sky brushes
		const char *skyStr = LB_GetKeyValue( entity, "_sky" );
		if ( skyStr && atoi( skyStr ) != 0 )
		{
			m_numSkippedLights++;
			return;
		}

		const char *targetStr = LB_GetKeyValue( entity, "target" );

		if ( targetStr && targetStr[0] )
		{
			for ( qEntity_s *target = entity->m_ownerWorld ? entity->m_ownerWorld->m_entityList : NULL; target; target = target->next )
			{
				if ( !target->m_targetName || stricmp( target->m_targetName, targetStr ) )
					continue;

				vec3_t dir = target->m_vecOrigin - origin;
				VectorNormalize( dir );

				light.m_direction = dir;
				break;
			}
		}

		if ( light.m_direction.x == 0.f && light.m_direction.y == 0.f && light.m_direction.z == 0.f )
		{
			vec3_t forward;
			LB_AngleVectors( angles, forward );
			VectorNormalize( forward );
			light.m_direction = forward;
		}

		light.m_stopdot = cosf( cone * (float)( M_PI / 180.0 ) );
		light.m_stopdot2 = cosf( cone2 * (float)( M_PI / 180.0 ) );
	}

	m_lights.push_back( light );
}

void CMapGeometry::AddFace( qEntity_s *entity, qBrush_s *brush, qFace_s *face )
{
	if ( !face->m_vertices || face->m_vertexCount < 3 )
		return;

	lbFace_t out;

	out.m_face = face;
	out.m_brush = brush;
	out.m_entity = entity;

	out.m_faceId = face->m_editorId;
	out.m_brushId = brush->m_editorId;
	out.m_entityId = entity->m_editorId;

	out.m_normal = face->m_plane.normal;

	vec3_t normal = out.m_normal;
	VectorNormalize( normal );

	out.m_normal = normal;
	out.m_planeDist = face->m_plane.dist;

	strncpy( out.m_texName, face->m_texDef.m_textureName, sizeof( out.m_texName ) - 1 );

	// a map saved while a preview was active can carry our generated texture names
	{
		char recovered[MAX_PATH] = { 0 };

		if ( LB_ParseBakedName( out.m_texName, recovered, sizeof( recovered ) ) && g_textureCache.Find( recovered ) )
		{
			LB_LogMsg( "geometry: face '%s' carried a baked name, using '%s'", out.m_texName, recovered );

			memset( out.m_texName, 0, sizeof( out.m_texName ) );
			strncpy( out.m_texName, recovered, sizeof( out.m_texName ) - 1 );
		}
	}

	out.m_sky = LB_IsSkyTexture( out.m_texName );
	out.m_liquid = LB_IsLiquidTexture( out.m_texName );

	bool utility = LB_IsUtilityTexture( out.m_texName );

	bool transparentEntity = LB_EntityDoesNotBlockLight( entity );

	out.m_occluder = !utility && !out.m_liquid && !transparentEntity;

	// sky and utility surfaces get no lightmap, hlrad does the same
	out.m_lit = !utility && !out.m_sky && !transparentEntity;

	if ( transparentEntity )
	{
		static int s_transparentLogged = 0;

		if ( s_transparentLogged < 8 )
		{
			s_transparentLogged++;
			LB_LogMsg( "geometry: face '%s' of '%s' skipped (non solid / transparent, rendermode=%d)", out.m_texName,
				entity && entity->m_className ? entity->m_className : "?", entity ? entity->m_renderMode.m_renderMode : 0 );
		}
	}

	{
		static int s_skippedLogged = 0;

		if ( !out.m_lit && s_skippedLogged < 10 )
		{
			s_skippedLogged++;
			LB_LogMsg( "geometry: face '%s' not lit (utility=%d sky=%d)", out.m_texName, utility ? 1 : 0, out.m_sky ? 1 : 0 );
		}
	}

	const lbTexture_t *texture = g_textureCache.Find( out.m_texName );

	if ( texture )
	{
		out.m_texWidth = texture->m_width;
		out.m_texHeight = texture->m_height;

		double r = 0, g = 0, b = 0;
		int count = texture->m_width * texture->m_height;

		for ( int i = 0; i < count; i++ )
		{
			r += texture->m_pixels[i * 3 + 0];
			g += texture->m_pixels[i * 3 + 1];
			b += texture->m_pixels[i * 3 + 2];
		}

		if ( count > 0 )
			out.m_albedo = vec3_t( (float)( r / count / 255.0 ), (float)( g / count / 255.0 ), (float)( b / count / 255.0 ) );
		else
			out.m_albedo = vec3_t( 0.5f, 0.5f, 0.5f );
	}
	else
	{
		out.m_texWidth = 64;
		out.m_texHeight = 64;
		out.m_albedo = vec3_t( 0.5f, 0.5f, 0.5f );
	}

	vec3_t e1, e2;
	LB_MakeBasis( normal, e1, e2 );

	out.m_e1 = e1;
	out.m_e2 = e2;

	out.m_verts.resize( face->m_vertexCount );
	out.m_uv.resize( face->m_vertexCount );

	lbVec2 bbMin( 1e30f, 1e30f );
	lbVec2 bbMax( -1e30f, -1e30f );

	for ( int i = 0; i < face->m_vertexCount; i++ )
	{
		const qVertex_t &vertex = face->m_vertices[i];

		out.m_verts[i] = vertex.coords;

		float x = DotProduct( vertex.coords, e1 );
		float y = DotProduct( vertex.coords, e2 );

		bbMin.x = std::min( bbMin.x, x );
		bbMin.y = std::min( bbMin.y, y );
		bbMax.x = std::max( bbMax.x, x );
		bbMax.y = std::max( bbMax.y, y );

		out.m_uv[i] = lbVec2( vertex.uv.x, vertex.uv.y );
	}

	out.m_bbMin = bbMin;
	out.m_bbMax = bbMax;

	// the editor fills the vertex uv itself when it builds a face. If the mapping came out
	{
		float minU = 1e30f, maxU = -1e30f, minV = 1e30f, maxV = -1e30f;

		for ( size_t i = 0; i < out.m_uv.size(); i++ )
		{
			minU = std::min( minU, out.m_uv[i].x );
			maxU = std::max( maxU, out.m_uv[i].x );
			minV = std::min( minV, out.m_uv[i].y );
			maxV = std::max( maxV, out.m_uv[i].y );
		}

		if ( ( maxU - minU ) < 1e-3f && ( maxV - minV ) < 1e-3f )
		{
			const qTexDef_t &texDef = face->m_texDef;

			float scaleX = ( texDef.m_scale.x != 0.f ) ? texDef.m_scale.x : 1.f;
			float scaleY = ( texDef.m_scale.y != 0.f ) ? texDef.m_scale.y : 1.f;

			vec3_t uAxis( texDef.m_UAxis.x, texDef.m_UAxis.y, texDef.m_UAxis.z );
			vec3_t vAxis( texDef.m_VAxis.x, texDef.m_VAxis.y, texDef.m_VAxis.z );

			for ( size_t i = 0; i < out.m_uv.size(); i++ )
			{
				out.m_uv[i] = lbVec2( DotProduct( out.m_verts[i], uAxis ) / scaleX + texDef.m_UAxis.w,
					DotProduct( out.m_verts[i], vAxis ) / scaleY + texDef.m_VAxis.w );
			}
		}
	}

	{
		vec3_t origin = out.m_verts[0];

		double suu = 0, suv = 0, svv = 0, bu = 0, bv = 0, b = 0; // normal equations
		double ru = 0, rv = 0;

		lbVec2 uv0 = out.m_uv[0];

		for ( int i = 1; i < face->m_vertexCount; i++ )
		{
			float dx = DotProduct( out.m_verts[i] - origin, e1 );
			float dy = DotProduct( out.m_verts[i] - origin, e2 );

			double du = out.m_uv[i].x - uv0.x;
			double dv = out.m_uv[i].y - uv0.y;

			suu += dx * dx;
			suv += dx * dy;
			svv += dy * dy;

			bu += dx * du;
			bv += dy * du;

			ru += dx * dv;
			rv += dy * dv;

			b += 1.0;
		}

		double det = suu * svv - suv * suv;

		if ( fabs( det ) > 1e-6 )
		{
			double invDet = 1.0 / det;

			out.m_uvGradU = lbVec2( (float)( ( svv * bu - suv * bv ) * invDet ), (float)( ( suu * bv - suv * bu ) * invDet ) );
			out.m_uvGradV = lbVec2( (float)( ( svv * ru - suv * rv ) * invDet ), (float)( ( suu * rv - suv * ru ) * invDet ) );
			out.m_uvOffset = lbVec2( (float)( uv0.x - out.m_uvGradU.x * DotProduct( origin, e1 ) - out.m_uvGradU.y * DotProduct( origin, e2 ) ),
				(float)( uv0.y - out.m_uvGradV.x * DotProduct( origin, e1 ) - out.m_uvGradV.y * DotProduct( origin, e2 ) ) );
		}
		else
		{
			out.m_uvGradU = lbVec2( 1.f, 0.f );
			out.m_uvGradV = lbVec2( 0.f, 1.f );
			out.m_uvOffset = lbVec2( -bbMin.x, -bbMin.y );
		}

		// editor uv is in texture repeats, the texdef is in texels, detect which one we got
		{
			float texScaleX = ( face->m_texDef.m_scale.x != 0.f ) ? face->m_texDef.m_scale.x : 1.f;
			float texScaleY = ( face->m_texDef.m_scale.y != 0.f ) ? face->m_texDef.m_scale.y : 1.f;

			float expectedU = 1.f / texScaleX;
			float expectedV = 1.f / texScaleY;

			float magnitudeU = sqrtf( out.m_uvGradU.x * out.m_uvGradU.x + out.m_uvGradU.y * out.m_uvGradU.y );
			float magnitudeV = sqrtf( out.m_uvGradV.x * out.m_uvGradV.x + out.m_uvGradV.y * out.m_uvGradV.y );

			float factorU = 1.f;
			float factorV = 1.f;

			if ( out.m_texWidth > 0 )
			{
				float asTexels = fabsf( magnitudeU - expectedU );
				float asRepeats = fabsf( magnitudeU * (float)out.m_texWidth - expectedU );

				if ( asRepeats < asTexels )
					factorU = (float)out.m_texWidth;
			}

			if ( out.m_texHeight > 0 )
			{
				float asTexels = fabsf( magnitudeV - expectedV );
				float asRepeats = fabsf( magnitudeV * (float)out.m_texHeight - expectedV );

				if ( asRepeats < asTexels )
					factorV = (float)out.m_texHeight;
			}

			if ( factorU != 1.f || factorV != 1.f )
			{
				out.m_uvGradU = lbVec2( out.m_uvGradU.x * factorU, out.m_uvGradU.y * factorU );
				out.m_uvGradV = lbVec2( out.m_uvGradV.x * factorV, out.m_uvGradV.y * factorV );
				out.m_uvOffset = lbVec2( out.m_uvOffset.x * factorU, out.m_uvOffset.y * factorV );
			}
		}
	}

	out.m_chop = 16.f;
	out.m_lmW = 0;
	out.m_lmH = 0;

	if ( out.m_lit )
		m_numLitFaces++;

	m_faces.push_back( out );
}

bool CMapGeometry::Build( qWorld_s *world )
{
	Purge();

	if ( !world )
		return false;

	std::vector<lbTri_t> tris;

	for ( qEntity_s *entity = world->m_entityList; entity; entity = entity->next )
	{
		AddLight( entity );
		AddSurfaceLight( entity );

		// one line per brush entity so the render mode handling can be verified from the log
		if ( entity->m_brushList )
		{
			static int s_entityLogged = 0;

			if ( s_entityLogged < 200 )
			{
				s_entityLogged++;

				const char *renderModeKey = LB_GetKeyValue( entity, "rendermode" );
				const char *renderAmtKey = LB_GetKeyValue( entity, "renderamt" );

				LB_LogMsg( "entity: '%s' renderMode=%d renderFlags=0x%x alpha=%d fx=%d keyMode='%s' keyAmt='%s' skip=%d",
					entity->m_className ? entity->m_className : "?",
					entity->m_renderMode.m_renderMode,
					entity->m_renderMode.m_renderFlags,
					(int)entity->m_renderMode.m_renderColor.a,
					entity->m_renderMode.m_renderFx,
					renderModeKey ? renderModeKey : "",
					renderAmtKey ? renderAmtKey : "",
					LB_EntityDoesNotBlockLight( entity ) ? 1 : 0 );
			}
		}

		for ( qBrush_s *brush = entity->m_brushList; brush; brush = brush->next )
		{
			for ( qFace_s *face = brush->m_faceList; face; face = face->next )
			{
				AddFace( entity, brush, face );
			}
		}
	}

	for ( size_t i = 0; i < m_faces.size(); i++ )
	{
		lbFace_t &face = m_faces[i];

		if ( !face.m_occluder || face.m_verts.size() < 3 )
			continue;

		for ( size_t k = 1; k + 1 < face.m_verts.size(); k++ )
		{
			lbTri_t tri;
			memset( &tri, 0, sizeof( tri ) );

			tri.m_v0 = face.m_verts[0];
			tri.m_v1 = face.m_verts[k];
			tri.m_v2 = face.m_verts[k + 1];

			tri.m_normal = face.m_normal;
			tri.m_planeDist = face.m_planeDist;
			tri.m_faceIndex = (int)i;

			tris.push_back( tri );

			m_numOccluders++;
		}
	}

	m_bvh.Build( tris );

	return m_numLitFaces > 0;
}

void CMapGeometry::BuildLightmaps( int chop )
{
	if ( chop < 1 )
		chop = 16;

	// a single unsplit brush face can be thousands of units across. Bound the total number
	// of lightmap samples and widen the spacing instead of trying to allocate gigabytes
	const long long kMaxLuxels = 2000000;

	int rounds = 0;

	for ( ;; )
	{
		long long total = 0;

		for ( size_t i = 0; i < m_faces.size(); i++ )
		{
			lbFace_t &face = m_faces[i];

			face.m_chop = (float)chop;

			if ( !face.m_lit )
			{
				face.m_lmW = 0;
				face.m_lmH = 0;
				continue;
			}

			float minX = floorf( face.m_bbMin.x / chop ) * chop;
			float minY = floorf( face.m_bbMin.y / chop ) * chop;

			int w = (int)floorf( ( face.m_bbMax.x - minX ) / chop ) + 1;
			int h = (int)floorf( ( face.m_bbMax.y - minY ) / chop ) + 1;

			if ( w < 2 )
				w = 2;
			if ( h < 2 )
				h = 2;

			if ( w > 256 )
				w = 256;
			if ( h > 256 )
				h = 256;

			face.m_lmMin = lbVec2( minX, minY );
			face.m_lmW = w;
			face.m_lmH = h;

			total += (long long)w * (long long)h;
		}

		if ( total <= kMaxLuxels || chop >= 256 || rounds >= 8 )
			break;

		chop *= 2;
		rounds++;

		LB_LogMsg( "geometry: %lld luxels is too many, chop raised to %d", total, chop );
	}

	m_numLuxels = 0;

	for ( size_t i = 0; i < m_faces.size(); i++ )
	{
		lbFace_t &face = m_faces[i];

		if ( !face.m_lit || face.m_lmW <= 0 )
			continue;

		int w = face.m_lmW;
		int h = face.m_lmH;

		size_t count = (size_t)w * (size_t)h;

		face.m_luxelPos.resize( count );
		face.m_luxelDirect.assign( count, vec3_t( 0.f, 0.f, 0.f ) );
		face.m_luxelLight.assign( count, vec3_t( 0.f, 0.f, 0.f ) );

		float minX = face.m_lmMin.x;
		float minY = face.m_lmMin.y;

		vec3_t origin = face.m_verts[0];

		for ( int y = 0; y < h; y++ )
		{
			for ( int x = 0; x < w; x++ )
			{
				float px = minX + x * chop;
				float py = minY + y * chop;

				size_t index = (size_t)y * w + x;

				vec3_t pos = origin + face.m_e1 * ( px - DotProduct( origin, face.m_e1 ) ) + face.m_e2 * ( py - DotProduct( origin, face.m_e2 ) );

				float dist = LB_DistToPlane( face.m_normal, face.m_planeDist, pos );
				pos -= face.m_normal * dist;

				face.m_luxelPos[index] = pos;
			}
		}

		m_numLuxels += (int)count;
	}
}

// bacontsu: light_surface, the newest way of doing texture lights
void CMapGeometry::AddSurfaceLight( qEntity_s *entity )
{
	if ( !entity->m_className || stricmp( entity->m_className, "light_surface" ) )
		return;

	lbSurfaceLight_t light;

	memset( &light, 0, sizeof( light ) );

	const char *value = LB_GetKeyValue( entity, "_tex" );

	if ( value )
		strncpy( light.m_texture, value, sizeof( light.m_texture ) - 1 );

	value = LB_GetKeyValue( entity, "_fclass" );

	if ( value )
		strncpy( light.m_classFilter, value, sizeof( light.m_classFilter ) - 1 );

	value = LB_GetKeyValue( entity, "_fname" );

	if ( value )
		strncpy( light.m_nameFilter, value, sizeof( light.m_nameFilter ) - 1 );

	vec3_t color;
	float brightness;

	LB_ParseLightValue( LB_GetKeyValue( entity, "_light" ), color, brightness );

	light.m_color = color * ( brightness / 255.f );

	light.m_scale = 1.f;

	value = LB_GetKeyValue( entity, "_scale" );

	if ( value )
		light.m_scale = (float)atof( value );

	vec3_t texColor;
	float texBrightness;

	if ( LB_ParseLightValue( LB_GetKeyValue( entity, "_texcolor" ), texColor, texBrightness ) )
	{
		light.m_texColor = texColor * ( texBrightness / 255.f );
		light.m_hasTexColor = true;
	}

	float cone = 0.f;
	float cone2 = 0.f;

	value = LB_GetKeyValue( entity, "_cone" );

	if ( value )
		cone = (float)atof( value );

	value = LB_GetKeyValue( entity, "_cone2" );

	if ( value )
		cone2 = (float)atof( value );

	if ( cone2 < cone )
		std::swap( cone, cone2 );

	// a cone of 90 degrees or more is just an omnidirectional emitter
	if ( cone > 0.f && cone < 90.f )
		light.m_stopdot = cosf( cone * (float)( M_PI / 180.0 ) );

	if ( cone2 > 0.f && cone2 < 90.f )
		light.m_stopdot2 = cosf( cone2 * (float)( M_PI / 180.0 ) );

	value = LB_GetKeyValue( entity, "_frange" );

	if ( value )
		light.m_range = (float)atof( value );

	m_surfaceLights.push_back( light );
}

// bacontsu: texture lights become area light patches, like hlrad
void CMapGeometry::BuildEmitters( int texChop )
{
	m_emitters.clear();
	m_numTexLightFaces = 0;

	if ( texChop < 4 )
		texChop = 32;

	const float kInvPi = 1.f / (float)M_PI;
	const int kMaxPatchesPerFace = 16;

	for ( size_t f = 0; f < m_faces.size(); f++ )
	{
		lbFace_t &face = m_faces[f];

		if ( !face.m_lit || face.m_liquid || face.m_verts.size() < 3 )
			continue;

		vec3_t color( 0.f, 0.f, 0.f );
		float scale = 1.f;
		float stopdot = 0.f;
		float stopdot2 = 0.f;
		float range = 0.f;
		bool fromTexture = false;
		bool found = false;

		for ( size_t s = 0; s < m_surfaceLights.size() && !found; s++ )
		{
			const lbSurfaceLight_t &surface = m_surfaceLights[s];

			if ( surface.m_texture[0] && stricmp( surface.m_texture, face.m_texName ) )
				continue;

			if ( surface.m_classFilter[0] && face.m_entity && face.m_entity->m_className
				&& stricmp( surface.m_classFilter, face.m_entity->m_className ) )
				continue;

			if ( surface.m_nameFilter[0] && face.m_entity && face.m_entity->m_targetName
				&& stricmp( surface.m_nameFilter, face.m_entity->m_targetName ) )
				continue;

			color = surface.m_color;
			scale = surface.m_scale;
			stopdot = surface.m_stopdot;
			stopdot2 = surface.m_stopdot2;
			range = surface.m_range;

			if ( surface.m_hasTexColor )
			{
				color = surface.m_texColor;
				fromTexture = false;
			}

			found = true;
		}

		if ( !found )
			found = LB_FindTexLight( face.m_texName, color, fromTexture );

		if ( !found || scale <= 0.f )
			continue;

		vec3_t baselight;

		if ( fromTexture )
		{
			// info_texlights only gives a brightness, the color comes from the texture
			baselight = face.m_albedo * color.x;
		}
		else
		{
			// explicit color, hlrad multiplies it by the surface reflectivity
			baselight = color * face.m_albedo;
		}

		baselight = baselight * scale;

		if ( baselight.x <= 0.f && baselight.y <= 0.f && baselight.z <= 0.f )
			continue;

		face.m_texLight = true;
		face.m_texLightColor = baselight;
		m_numTexLightFaces++;

		float extentU = face.m_bbMax.x - face.m_bbMin.x;
		float extentV = face.m_bbMax.y - face.m_bbMin.y;

		if ( extentU < 1.f || extentV < 1.f )
			continue;

		int numU = (int)ceilf( extentU / (float)texChop );
		int numV = (int)ceilf( extentV / (float)texChop );

		numU = std::max( 1, std::min( numU, 8 ) );
		numV = std::max( 1, std::min( numV, 8 ) );

		while ( numU * numV > kMaxPatchesPerFace )
		{
			if ( numU >= numV )
				numU = std::max( 1, numU / 2 );
			else
				numV = std::max( 1, numV / 2 );
		}

		float patchU = extentU / (float)numU;
		float patchV = extentV / (float)numV;
		float patchArea = patchU * patchV;

		vec3_t origin = face.m_verts[0];

		for ( int j = 0; j < numV; j++ )
		{
			for ( int i = 0; i < numU; i++ )
			{
				float cu = face.m_bbMin.x + ( i + 0.5f ) * patchU;
				float cv = face.m_bbMin.y + ( j + 0.5f ) * patchV;

				vec3_t pos = origin + face.m_e1 * ( cu - DotProduct( origin, face.m_e1 ) ) + face.m_e2 * ( cv - DotProduct( origin, face.m_e2 ) );

				float dist = DotProduct( face.m_normal, pos ) - face.m_planeDist;
				pos -= face.m_normal * dist;

				lbEmitter_t emitter;
				memset( &emitter, 0, sizeof( emitter ) );

				emitter.m_origin = pos + face.m_normal * 1.f;
				emitter.m_normal = face.m_normal;
				emitter.m_power = baselight * kInvPi;
				emitter.m_area = patchArea;
				emitter.m_stopdot = stopdot;
				emitter.m_stopdot2 = stopdot2;
				emitter.m_range = range;

				// past this distance the contribution drops below the cut off in rad.cpp
				vec3_t maxAdd = baselight * ( patchArea * kInvPi ) * ( 1.f / 0.05f );
				float strongest = std::max( maxAdd.x, std::max( maxAdd.y, maxAdd.z ) );
				float reach = ( strongest > 0.f ) ? sqrtf( strongest ) : 0.f;

				emitter.m_reach = std::min( reach * reach, 8192.f * 8192.f );

				m_emitters.push_back( emitter );
			}
		}
	}

	if ( m_numTexLightFaces > 0 )
		LB_LogMsg( "texlights: %d emitting faces, %d emitters", m_numTexLightFaces, (int)m_emitters.size() );
}

void LB_DefaultSettings( lbSettings_t &settingsOut )
{
	memset( &settingsOut, 0, sizeof( settingsOut ) );

	settingsOut.m_chop = 16;
	settingsOut.m_bounce = 4;
	settingsOut.m_bounceEnabled = true;
	settingsOut.m_bounceColorScale = 0.f;
	settingsOut.m_lightScale = 2.f;
	settingsOut.m_skyScale = 1.f;
	settingsOut.m_lightGamma = 0.5f;
	settingsOut.m_limitThreshold = 188.f;
	settingsOut.m_minLight = 0.f;
	settingsOut.m_bounceOnlySky = true;
	settingsOut.m_numThreads = 0;
	settingsOut.m_texLights = true;
	settingsOut.m_texChop = 32;
	settingsOut.m_preview = true;
	settingsOut.m_restoreOnClick = true;
	settingsOut.m_overrideUVs = true;
	settingsOut.m_renameTextures = false;
	settingsOut.m_verbose = true;
}
