//=====================================================================================//
// bacontsu: the light compiler
//=====================================================================================//
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <algorithm>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>

#include "lightbake.h"

#define LB_NORMAL_EPSILON		1e-5f
#define LB_MAXLIGHT				255.0f
#define LB_RAY_OFFSET			0.75f

static inline float LB_MaxComponent( const vec3_t &v )
{
	return std::max( v.x, std::max( v.y, v.z ) );
}

static inline float LB_Luminance( const vec3_t &v )
{
	return 0.299f * v.x + 0.587f * v.y + 0.114f * v.z;
}

static int LB_GetNumThreads( const lbSettings_t &settings )
{
	if ( settings.m_numThreads > 0 )
		return settings.m_numThreads;

	unsigned int hardware = std::thread::hardware_concurrency();

	if ( hardware == 0 )
		return 4;

	return (int)std::min( hardware, 32u );
}

// bacontsu: runs fn( index, threadIndex ) for every index in [0, count) on numThreads threads
template <typename Fn>
static void LB_ParallelFor( int count, int numThreads, const std::function<void( int, int, const char * )> &progressFn, const char *label, Fn fn )
{
	if ( count <= 0 )
		return;

	if ( numThreads < 1 )
		numThreads = 1;

	if ( numThreads > count )
		numThreads = count;

	std::atomic<int> next( 0 );
	std::atomic<int> done( 0 );

	std::vector<std::thread> threads;
	threads.reserve( numThreads );

	for ( int t = 0; t < numThreads; t++ )
	{
		threads.push_back( std::thread( [&, t]()
		{
			for ( ;; )
			{
				int index = next.fetch_add( 1 );

				if ( index >= count )
					break;

				fn( index, t );

				done.fetch_add( 1 );
			}
		} ) );
	}

	while ( done.load() < count )
	{
		if ( progressFn )
			progressFn( done.load(), count, label );

		std::this_thread::sleep_for( std::chrono::milliseconds( 40 ) );
	}

	for ( size_t i = 0; i < threads.size(); i++ )
		threads[i].join();

	if ( progressFn )
		progressFn( count, count, label );
}

// bacontsu: cheap deterministic RNG (xorshift32), seeded per thread so results are reproducible
struct lbRng_t
{
	unsigned int m_state;

	explicit lbRng_t( unsigned int seed ) { m_state = seed ? seed : 0x9E3779B9u; }

	unsigned int Next()
	{
		unsigned int x = m_state;
		x ^= x << 13;
		x ^= x >> 17;
		x ^= x << 5;
		m_state = x;
		return x;
	}

	float NextFloat()
	{
		return (float)( Next() & 0xFFFFFF ) / (float)0x1000000;
	}
};

// builds an orthonormal basis around the given normal
static void LB_BuildHemisphereBasis( const vec3_t &normal, vec3_t &t, vec3_t &b )
{
	// duff et al. branchless ONB
	float sign = ( normal.z >= 0.f ) ? 1.f : -1.f;
	float a = -1.f / ( sign + normal.z );
	float c = normal.x * normal.y * a;

	t = vec3_t( 1.f + sign * normal.x * normal.x * a, sign * c, -sign * normal.x );
	b = vec3_t( c, sign + normal.y * normal.y * a, -normal.y );
}

static bool LB_PointVisible( const CMapGeometry &geometry, const vec3_t &from, const vec3_t &normal, const vec3_t &to )
{
	vec3_t delta = to - from;
	float dist = VectorNormalize( delta );

	if ( dist <= LB_RAY_OFFSET )
		return true;

	vec3_t start = from + normal * LB_RAY_OFFSET;

	return !geometry.m_bvh.IsBlocked( start, delta, dist );
}

#define LB_DOME_SAMPLES		16

// bacontsu: sky lit means the ray reaches a sky surface, escaping the map does not count
static bool LB_SkyVisible( const CMapGeometry &geometry, const vec3_t &pos, const vec3_t &dir )
{
	int triIndex = -1;

	float dist = geometry.m_bvh.Trace( pos, dir, 131072.f, &triIndex );

	if ( dist <= 0.f )
		return false;

	int faceIndex = geometry.m_bvh.GetTriangleFace( triIndex );

	if ( faceIndex < 0 || faceIndex >= (int)geometry.m_faces.size() )
		return false;

	return geometry.m_faces[faceIndex].m_sky;
}

// bacontsu: hlrad's skylight, sun term is just the cosine with no falloff, the dome is the
// whole sphere integrated and comes out at about 1/8 of _diffuse_light for open sky
static vec3_t LB_ComputeSkyLight( const CMapGeometry &geometry, const lbSettings_t &settings, const lbFace_t &face, const vec3_t &pos, unsigned int seed )
{
	if ( !geometry.m_sky.m_enabled )
		return vec3_t( 0.f, 0.f, 0.f );

	vec3_t sunDir = -geometry.m_sky.m_direction;
	VectorNormalize( sunDir );

	vec3_t origin = pos + face.m_normal * LB_RAY_OFFSET;

	vec3_t sum( 0.f, 0.f, 0.f );

	float dotSun = DotProduct( face.m_normal, sunDir );

	if ( dotSun > (float)1e-5 )
	{
		if ( LB_SkyVisible( geometry, origin, sunDir ) )
			sum += geometry.m_sky.m_color * dotSun;
	}

	if ( settings.m_skyScale > 0.f )
	{
		vec3_t tangent, bitangent;
		LB_BuildHemisphereBasis( face.m_normal, tangent, bitangent );

		lbRng_t rng( seed );

		int visible = 0;

		for ( int s = 0; s < LB_DOME_SAMPLES; s++ )
		{
			float r1 = rng.NextFloat();
			float r2 = rng.NextFloat();

			float phi = 2.f * (float)M_PI * r1;
			float cosTheta = sqrtf( 1.f - r2 );
			float sinTheta = sqrtf( r2 );

			vec3_t dir = tangent * ( sinTheta * cosf( phi ) ) + bitangent * ( sinTheta * sinf( phi ) ) + face.m_normal * cosTheta;

			VectorNormalize( dir );

			if ( LB_SkyVisible( geometry, origin, dir ) )
				visible++;
		}

		float skyFraction = (float)visible / (float)LB_DOME_SAMPLES;

		// diffuse * sky/2 * ( 1/4 ) * skyFraction
		sum += geometry.m_sky.m_diffuse * ( settings.m_skyScale * skyFraction * 0.125f );
	}

	return sum;
}

static vec3_t LB_ComputeDirect( const CMapGeometry &geometry, const lbSettings_t &settings, const lbFace_t &face, const vec3_t &pos, unsigned int seed )
{
	vec3_t sum( 0.f, 0.f, 0.f );

	for ( size_t i = 0; i < geometry.m_lights.size(); i++ )
	{
		const lbLight_t &light = geometry.m_lights[i];

		vec3_t delta = light.m_origin - pos;

		float dist = VectorNormalize( delta );

		float dotNormal = DotProduct( delta, face.m_normal );

		if ( dotNormal <= LB_NORMAL_EPSILON )
			continue;

		if ( dist < 1.f )
			dist = 1.f;

		// hlrad: ratio = dot / ( dist * fade ), falloff 2 divides by dist again
		float denominator = dist;

		if ( light.m_falloff == LB_FALLOFF_INVERSE_SQUARE )
			denominator *= dist;

		float ratio = dotNormal / denominator;

		if ( light.m_spot )
		{
			// hlrad: dot2 = -DotProduct( delta, light->normal )
			float dotCone = -DotProduct( delta, light.m_direction );

			if ( dotCone <= light.m_stopdot2 )
				continue;

			ratio *= dotCone;

			if ( dotCone <= light.m_stopdot )
			{
				float softness = ( dotCone - light.m_stopdot2 ) / ( light.m_stopdot - light.m_stopdot2 );
				ratio *= softness;
			}
		}

		if ( light.m_radius > 0.f && dist > light.m_radius )
			continue;

		if ( !LB_PointVisible( geometry, pos, face.m_normal, light.m_origin ) )
			continue;

		sum += light.m_color * ratio;
	}

	sum += LB_ComputeSkyLight( geometry, settings, face, pos, seed );

	// bacontsu: texture lights, each emitter is one patch of an emitting face
	for ( size_t i = 0; i < geometry.m_emitters.size(); i++ )
	{
		const lbEmitter_t &emitter = geometry.m_emitters[i];

		vec3_t delta = emitter.m_origin - pos;

		float dist2 = DotProduct( delta, delta );

		// far emitters can never reach the cut off below, skip the sqrt
		if ( dist2 > emitter.m_reach )
			continue;

		float dist = sqrtf( dist2 );

		if ( dist < 1.f )
			dist = 1.f;

		vec3_t dir = delta * ( 1.f / dist );

		float dotReceiver = DotProduct( dir, face.m_normal );

		if ( dotReceiver <= LB_NORMAL_EPSILON )
			continue;

		float dotEmitter = -DotProduct( dir, emitter.m_normal );

		// receiver sits on the emitter plane, this also stops the face lighting itself
		if ( dotEmitter * dist <= 0.04f )
			continue;

		if ( emitter.m_stopdot2 > 0.f && dotEmitter <= emitter.m_stopdot2 )
			continue;

		if ( emitter.m_range > 0.f && dist > emitter.m_range )
			continue;

		float ratio = dotReceiver * dotEmitter / ( dist * dist );

		// hlrad caps the contribution per unit area to avoid hot spots at corners
		if ( ratio * emitter.m_area > 0.4f )
			ratio = 0.4f / emitter.m_area;

		if ( emitter.m_stopdot > 0.f && dotEmitter <= emitter.m_stopdot )
			ratio *= ( dotEmitter - emitter.m_stopdot2 ) / ( emitter.m_stopdot - emitter.m_stopdot2 );

		vec3_t add = emitter.m_power * ( emitter.m_area * ratio );

		// cheap reject before paying for a ray
		if ( LB_MaxComponent( add ) < 0.05f )
			continue;

		if ( !LB_PointVisible( geometry, pos, face.m_normal, emitter.m_origin ) )
			continue;

		sum += add;
	}

	return sum;
}

// bacontsu: samples the light that was computed during the previous bounce iteration
static vec3_t LB_SampleFaceGrid( const lbFace_t &face, const std::vector<vec3_t> &grid, const vec3_t &pos )
{
	if ( face.m_lmW <= 0 || face.m_lmH <= 0 || grid.empty() )
		return vec3_t( 0.f, 0.f, 0.f );

	float x = DotProduct( pos, face.m_e1 );
	float y = DotProduct( pos, face.m_e2 );

	float fx = ( x - face.m_lmMin.x ) / face.m_chop;
	float fy = ( y - face.m_lmMin.y ) / face.m_chop;

	int w = face.m_lmW;
	int h = face.m_lmH;

	fx = std::max( 0.f, std::min( fx, (float)( w - 1 ) ) );
	fy = std::max( 0.f, std::min( fy, (float)( h - 1 ) ) );

	int x0 = (int)floorf( fx );
	int y0 = (int)floorf( fy );

	int x1 = std::min( x0 + 1, w - 1 );
	int y1 = std::min( y0 + 1, h - 1 );

	float tx = fx - x0;
	float ty = fy - y0;

	vec3_t v00 = grid[(size_t)y0 * w + x0];
	vec3_t v10 = grid[(size_t)y0 * w + x1];
	vec3_t v01 = grid[(size_t)y1 * w + x0];
	vec3_t v11 = grid[(size_t)y1 * w + x1];

	vec3_t top = v00 * ( 1.f - tx ) + v10 * tx;
	vec3_t bottom = v01 * ( 1.f - tx ) + v11 * tx;

	return top * ( 1.f - ty ) + bottom * ty;
}

CRad::CRad( CMapGeometry &geometry, const lbSettings_t &settings )
	: m_geometry( geometry ), m_settings( settings )
{
	m_directTime = 0.f;
	m_bounceTime = 0.f;
	m_numPatches = 0;
}

void CRad::ComputeDirect()
{
	int numThreads = LB_GetNumThreads( m_settings );

	int faceCount = (int)m_geometry.m_faces.size();

	LB_ParallelFor( faceCount, numThreads, m_progressFn, "Computing direct lighting...", [&]( int faceIndex, int )
	{
		lbFace_t &face = m_geometry.m_faces[faceIndex];

		if ( !face.m_lit || face.m_luxelPos.empty() )
			return;

		for ( size_t i = 0; i < face.m_luxelPos.size(); i++ )
		{
			face.m_luxelDirect[i] = LB_ComputeDirect( m_geometry, m_settings, face, face.m_luxelPos[i],
				(unsigned int)( faceIndex * 7919 + (int)i * 104729 + 1 ) );
		}

		// a texture light face glows with at least its own brightness
		if ( face.m_texLight )
		{
			for ( size_t i = 0; i < face.m_luxelDirect.size(); i++ )
			{
				for ( int c = 0; c < 3; c++ )
				{
					if ( face.m_luxelDirect[i][c] < face.m_texLightColor[c] )
						face.m_luxelDirect[i][c] = face.m_texLightColor[c];
				}
			}
		}
	} );
}

// bacontsu: a cosine weighted hemisphere gather over the previously computed light
void CRad::ComputeBounce()
{
	if ( !m_settings.m_bounceEnabled || m_settings.m_bounce <= 0 )
		return;

	int numThreads = LB_GetNumThreads( m_settings );

	// samples per luxel. Later bounces carry little high frequency detail, so they get
	int samples = 32;

	if ( m_settings.m_bounce > 2 )
		samples = 16;

	std::vector<std::vector<vec3_t> > previous;

	previous.resize( m_geometry.m_faces.size() );

	for ( size_t i = 0; i < m_geometry.m_faces.size(); i++ )
	{
		previous[i] = m_geometry.m_faces[i].m_luxelDirect;
	}

	// the first iteration adds the direct light, so "previous" starts as direct only
	for ( size_t i = 0; i < m_geometry.m_faces.size(); i++ )
	{
		lbFace_t &face = m_geometry.m_faces[i];

		face.m_luxelLight = face.m_luxelDirect;
	}

	for ( int iteration = 0; iteration < m_settings.m_bounce; iteration++ )
	{
		int faceCount = (int)m_geometry.m_faces.size();

		LB_LogMsg( "rad: bounce %d/%d (samples=%d)", iteration + 1, m_settings.m_bounce, samples );

		clock_t iterationStart = clock();

		LB_ParallelFor( faceCount, numThreads, m_progressFn, "Gathering indirect light...", [&]( int faceIndex, int threadIndex )
		{
			lbFace_t &face = m_geometry.m_faces[faceIndex];

			if ( !face.m_lit || face.m_luxelPos.empty() )
				return;

			lbRng_t rng( (unsigned int)( threadIndex * 747796405u + iteration * 2891336453u + 12345u ) );

			vec3_t tangent, bitangent;
			LB_BuildHemisphereBasis( face.m_normal, tangent, bitangent );

			std::vector<vec3_t> bounce;
			bounce.assign( face.m_luxelPos.size(), vec3_t( 0.f, 0.f, 0.f ) );

			for ( size_t i = 0; i < face.m_luxelPos.size(); i++ )
			{
				const vec3_t &pos = face.m_luxelPos[i];

				vec3_t origin = pos + face.m_normal * LB_RAY_OFFSET;

				vec3_t accum( 0.f, 0.f, 0.f );

				// reflectivity of the surface we are sampling from
				vec3_t reflect( 0.5f, 0.5f, 0.5f );

				if ( m_settings.m_bounceColorScale > 0.f )
				{
					float s = std::min( m_settings.m_bounceColorScale, 1.f );
					reflect = vec3_t( 0.5f, 0.5f, 0.5f ) * ( 1.f - s ) + face.m_albedo * s;
				}

				for ( int s = 0; s < samples; s++ )
				{
					float r1 = rng.NextFloat();
					float r2 = rng.NextFloat();

					// cosine weighted hemisphere sample
					float phi = 2.f * (float)M_PI * r1;
					float cosTheta = sqrtf( 1.f - r2 );
					float sinTheta = sqrtf( r2 );

					vec3_t dir = tangent * ( sinTheta * cosf( phi ) ) + bitangent * ( sinTheta * sinf( phi ) ) + face.m_normal * cosTheta;

					VectorNormalize( dir );

					int triIndex = -1;

					float hitDist = m_geometry.m_bvh.Trace( origin, dir, 8000.f, &triIndex );

					if ( hitDist <= 0.f )
					{
						// escaped the map: treat as open sky if there is a light_environment
						if ( m_geometry.m_sky.m_enabled )
						{
							float dot = DotProduct( dir, -m_geometry.m_sky.m_direction );

							if ( dot > 0.f )
								accum += m_geometry.m_sky.m_color * ( dot * m_settings.m_skyScale );
						}

						continue;
					}

					int hitFaceIndex = m_geometry.m_bvh.GetTriangleFace( triIndex );

					if ( hitFaceIndex < 0 || hitFaceIndex >= (int)m_geometry.m_faces.size() )
						continue;

					const lbFace_t &hitFace = m_geometry.m_faces[hitFaceIndex];

					if ( hitFaceIndex >= (int)previous.size() || previous[hitFaceIndex].empty() )
						continue;

					vec3_t hitPos = origin + dir * hitDist;

					vec3_t hitLight = LB_SampleFaceGrid( hitFace, previous[hitFaceIndex], hitPos );

					if ( LB_MaxComponent( hitLight ) <= 0.f )
						continue;

					accum += hitLight * reflect;
				}

				// cosine weighted sampling: ( reflectivity / N ) * sum of incoming light
				bounce[i] = accum * ( 1.f / (float)samples );
			}

			for ( size_t i = 0; i < bounce.size(); i++ )
			{
				face.m_luxelLight[i] += bounce[i];
			}
		} );

		for ( size_t i = 0; i < m_geometry.m_faces.size(); i++ )
		{
			previous[i] = m_geometry.m_faces[i].m_luxelLight;
		}

		LB_LogMsg( "rad: bounce %d done (%.2fs)", iteration + 1, (double)( clock() - iterationStart ) / (double)CLOCKS_PER_SEC );
	}
}

// bacontsu: applies hlrad's final scaling: light scale, maximum clamp and light gamma
void CRad::FinishLightmaps()
{
	float lightScale = m_settings.m_lightScale;

	if ( lightScale <= 0.f )
		lightScale = 1.f;

	float gamma = m_settings.m_lightGamma;

	if ( gamma <= 0.f )
		gamma = 1.f;

	bool applyGamma = fabsf( gamma - 1.f ) > 0.001f;

	float limiter = m_settings.m_limitThreshold;
	float minlight = m_settings.m_minLight;

	for ( size_t f = 0; f < m_geometry.m_faces.size(); f++ )
	{
		lbFace_t &face = m_geometry.m_faces[f];

		if ( !face.m_lit || face.m_luxelLight.empty() )
			continue;

		for ( size_t i = 0; i < face.m_luxelLight.size(); i++ )
		{
			vec3_t &light = face.m_luxelLight[i];

			// hlrad: -scale is applied first
			light = light * lightScale;

			// hlrad: -minlight is a floor applied after scaling
			if ( minlight > 0.f )
			{
				for ( int c = 0; c < 3; c++ )
				{
					if ( light[c] < minlight )
						light[c] = minlight;
				}
			}

			// hlrad: gamma is applied as pow( L / 256, gamma ) * 256
			if ( applyGamma )
			{
				for ( int c = 0; c < 3; c++ )
				{
					if ( light[c] < 0.f )
						light[c] = 0.f;

					light[c] = powf( light[c] / 256.f, gamma ) * 256.f;
				}
			}

			// hlrad: -limiter clamps the brightest channel and keeps the hue
			float maximum = LB_MaxComponent( light );

			if ( limiter > 0.f && maximum > limiter )
				light = light * ( limiter / maximum );

			for ( int c = 0; c < 3; c++ )
			{
				if ( light[c] < 0.f )
					light[c] = 0.f;

				if ( light[c] > 255.f )
					light[c] = 255.f;
			}
		}
	}
}

bool CRad::Run( const std::function<void( int, int, const char * )> &progressFn )
{
	if ( m_geometry.m_faces.empty() )
		return false;

	m_numPatches = m_geometry.m_numLuxels;

	m_progressFn = progressFn;

	LB_LogMsg( "rad: start, faces=%d luxels=%d threads=%d", (int)m_geometry.m_faces.size(), m_numPatches, LB_GetNumThreads( m_settings ) );

	clock_t start = clock();

	ComputeDirect();

	LB_LogMsg( "rad: direct done (%.2fs)", (double)( clock() - start ) / (double)CLOCKS_PER_SEC );

	clock_t directEnd = clock();

	m_directTime = (float)( directEnd - start ) / (float)CLOCKS_PER_SEC;

	if ( progressFn )
		progressFn( 1, 1, "Computing indirect lighting..." );

	ComputeBounce();

	LB_LogMsg( "rad: bounce done" );

	clock_t bounceEnd = clock();

	m_bounceTime = (float)( bounceEnd - directEnd ) / (float)CLOCKS_PER_SEC;

	FinishLightmaps();

	return true;
}
