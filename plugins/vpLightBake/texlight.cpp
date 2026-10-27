//=====================================================================================//
// bacontsu: texture lights, lights.rad and info_texlights
//=====================================================================================//
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <vector>

#include "lightbake.h"

struct lbTexLightDef
{
	std::string m_name;
	vec3_t m_color;
	bool m_fromTexture;			// info_texlights, color comes from the texture itself
};

static std::vector<lbTexLightDef> s_texLights;

void LB_ClearTexLights()
{
	s_texLights.clear();
}

int LB_GetTexLightCount()
{
	return (int)s_texLights.size();
}

// bacontsu: later definitions win, same as hlrad
static void LB_AddTexLight( const char *name, const vec3_t &color, bool fromTexture )
{
	if ( !name || !name[0] )
		return;

	for ( size_t i = 0; i < s_texLights.size(); i++ )
	{
		if ( !stricmp( s_texLights[i].m_name.c_str(), name ) )
		{
			s_texLights[i].m_color = color;
			s_texLights[i].m_fromTexture = fromTexture;
			return;
		}
	}

	lbTexLightDef def;
	def.m_name = name;
	def.m_color = color;
	def.m_fromTexture = fromTexture;

	s_texLights.push_back( def );
}

// bacontsu: rad files often write "wadname/texture", the map only stores "texture"
static const char *LB_TextureBaseName( const char *name )
{
	const char *last = name;

	for ( const char *c = name; *c; c++ )
	{
		if ( *c == '/' || *c == '\\' )
			last = c + 1;
	}

	return last;
}

bool LB_FindTexLight( const char *textureName, vec3_t &colorOut, bool &fromTextureOut )
{
	if ( !textureName || !textureName[0] )
		return false;

	for ( size_t i = 0; i < s_texLights.size(); i++ )
	{
		const char *stored = s_texLights[i].m_name.c_str();

		if ( stricmp( stored, textureName ) && stricmp( LB_TextureBaseName( stored ), textureName ) )
			continue;

		colorOut = s_texLights[i].m_color;
		fromTextureOut = s_texLights[i].m_fromTexture;
		return true;
	}

	return false;
}

// bacontsu: a rad line is TEXNAME r g b [scale], or just a brightness
static bool LB_ParseRadLine( const std::vector<std::string> &tokens )
{
	if ( tokens.size() < 2 )
		return false;

	const std::string &name = tokens[0];

	if ( name.size() > 63 )
		return false;

	std::vector<float> numbers;

	for ( size_t i = 1; i < tokens.size() && numbers.size() < 4; i++ )
	{
		const char *text = tokens[i].c_str();

		char *end = NULL;
		float value = (float)strtod( text, &end );

		if ( end == text )
			return false;

		numbers.push_back( value );
	}

	vec3_t color;

	if ( numbers.size() == 1 )
	{
		color = vec3_t( numbers[0], numbers[0], numbers[0] );
	}
	else if ( numbers.size() >= 3 )
	{
		color = vec3_t( numbers[0], numbers[1], numbers[2] );

		if ( numbers.size() == 4 )
			color = color * ( numbers[3] / 255.f );
	}
	else
	{
		return false;
	}

	LB_AddTexLight( name.c_str(), color, false );

	return true;
}

int LB_LoadRadFile( const char *filePath )
{
	if ( !filePath || !filePath[0] )
		return 0;

	FILE *f = fopen( filePath, "rb" );

	if ( !f )
		return 0;

	int added = 0;

	char line[1024];

	while ( fgets( line, sizeof( line ), f ) )
	{
		char *comment = strstr( line, "//" );

		if ( comment )
			*comment = '\0';

		std::vector<std::string> tokens;
		char *cursor = line;

		while ( *cursor )
		{
			while ( *cursor && isspace( (unsigned char)*cursor ) )
				cursor++;

			if ( !*cursor )
				break;

			char *start = cursor;

			while ( *cursor && !isspace( (unsigned char)*cursor ) )
				cursor++;

			tokens.push_back( std::string( start, cursor - start ) );
		}

		if ( tokens.empty() )
			continue;

		if ( LB_ParseRadLine( tokens ) )
			added++;
	}

	fclose( f );

	if ( added > 0 )
		LB_LogMsg( "texlights: %s added %d textures", filePath, added );

	return added;
}

int LB_LoadRadDirectory( const char *dir )
{
	if ( !dir || !dir[0] )
		return 0;

	std::error_code ec;

	std::filesystem::path root( dir );

	if ( !std::filesystem::exists( root, ec ) || !std::filesystem::is_directory( root, ec ) )
		return 0;

	int added = 0;

	try
	{
		std::filesystem::directory_iterator it( root, std::filesystem::directory_options::skip_permission_denied, ec );
		std::filesystem::directory_iterator end;

		for ( ; it != end; it.increment( ec ) )
		{
			if ( ec )
				break;

			if ( !it->is_regular_file( ec ) )
				continue;

			std::string ext = it->path().extension().string();

			std::transform( ext.begin(), ext.end(), ext.begin(), []( unsigned char c ) { return (char)tolower( c ); } );

			if ( ext != ".rad" )
				continue;

			added += LB_LoadRadFile( it->path().string().c_str() );
		}
	}
	catch ( ... )
	{
	}

	return added;
}

// bacontsu: info_texlights wins over the rad files, colour comes from the texture
int LB_CollectInfoTexLights( qWorld_s *world )
{
	if ( !world )
		return 0;

	int added = 0;

	for ( qEntity_s *entity = world->m_entityList; entity; entity = entity->next )
	{
		if ( !entity->m_className || stricmp( entity->m_className, "info_texlights" ) )
			continue;

		for ( epair_s *pair = entity->epairs; pair; pair = pair->next )
		{
			if ( !pair->key || !pair->value || !pair->key[0] )
				continue;

			// the editor writes its own bookkeeping keys too
			if ( !stricmp( pair->key, "classname" ) || !stricmp( pair->key, "origin" ) || !stricmp( pair->key, "angle" ) )
				continue;

			float brightness = (float)atof( pair->value );

			if ( brightness <= 0.f )
				continue;

			LB_AddTexLight( pair->key, vec3_t( brightness, brightness, brightness ), true );
			added++;
		}
	}

	if ( added > 0 )
		LB_LogMsg( "texlights: info_texlights added %d textures", added );

	return added;
}
