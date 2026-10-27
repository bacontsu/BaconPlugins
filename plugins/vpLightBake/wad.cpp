//=====================================================================================//
// bacontsu: wad3 texture cache
//=====================================================================================//
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <filesystem>

#include "wad.h"

#include "vstdlib_static.h"

// defined in actions.cpp
void LB_LogMsg( const char *format, ... );

#define LB_MAX_LUMPS			20000
#define LB_MAX_TEXTURES			4000
#define LB_MAX_DECODED_BYTES	( 96 * 1024 * 1024 )

static std::string LB_Lower( const char *s )
{
	std::string out( s ? s : "" );

	std::transform( out.begin(), out.end(), out.begin(), []( unsigned char c ) { return (char)tolower( c ); } );

	return out;
}

CTextureCache::CTextureCache()
{
	m_decodedBytes = 0;
	m_haveSeenWad = false;
}

CTextureCache::~CTextureCache()
{
	Purge();
}

const lbTexture_t *CTextureCache::AddTexture( const char *name, int width, int height, const unsigned char *rgb )
{
	if ( width <= 0 || height <= 0 || !rgb )
		return NULL;

	if ( (int)m_textures.size() >= LB_MAX_TEXTURES )
		return NULL;

	size_t size = (size_t)width * (size_t)height * 3;

	if ( m_decodedBytes + size > LB_MAX_DECODED_BYTES )
		return NULL;

	lbTexture_t *texture = new lbTexture_t();

	if ( !texture )
		return NULL;

	memset( texture, 0, sizeof( lbTexture_t ) );

	strncpy( texture->m_name, name, sizeof( texture->m_name ) - 1 );

	texture->m_pixels = (unsigned char *)malloc( size );

	if ( !texture->m_pixels )
	{
		delete texture;
		return NULL;
	}

	memcpy( texture->m_pixels, rgb, size );

	texture->m_width = width;
	texture->m_height = height;

	m_decodedBytes += size;

	m_textures.push_back( texture );

	return texture;
}

// bacontsu: decodes one miptex into RGB24
bool CTextureCache::DecodeLump( int lumpIndex, lbTexture_t &textureOut )
{
	if ( lumpIndex < 0 || lumpIndex >= (int)m_lumps.size() )
		return false;

	const LumpRef &ref = m_lumps[lumpIndex];

	FILE *f = fopen( ref.m_wadPath.c_str(), "rb" );

	if ( !f )
		return false;

	long diskSize = ref.m_diskSize;

	if ( diskSize < (long)sizeof( lbMipTex_t ) || diskSize > 16 * 1024 * 1024 )
	{
		fclose( f );
		return false;
	}

	unsigned char *lump = (unsigned char *)malloc( (size_t)diskSize );

	if ( !lump )
	{
		fclose( f );
		return false;
	}

	bool ok = false;

	if ( fseek( f, ref.m_filePos, SEEK_SET ) == 0 && fread( lump, 1, (size_t)diskSize, f ) == (size_t)diskSize )
	{
		lbMipTex_t mip;
		memcpy( &mip, lump, sizeof( mip ) );

		if ( mip.width > 0 && mip.height > 0 && mip.width <= 4096 && mip.height <= 4096 && mip.offsets[0] != 0 )
		{
			int numPixels = (int)mip.width * (int)mip.height;

			int palOffset = (int)mip.offsets[3] + ( (int)mip.width / 8 ) * ( (int)mip.height / 8 );

			if ( (size_t)mip.offsets[0] + (size_t)numPixels <= (size_t)diskSize && (size_t)palOffset + 2 <= (size_t)diskSize )
			{
				short palSize = *(const short *)( lump + palOffset );

				if ( palSize >= 2 && palSize <= 256 && (size_t)( palOffset + 2 + palSize * 3 ) <= (size_t)diskSize )
				{
					const unsigned char *pal = lump + palOffset + 2;
					const unsigned char *indexed = lump + mip.offsets[0];

					unsigned char *rgb = (unsigned char *)malloc( (size_t)numPixels * 3 );

					if ( rgb )
					{
						for ( int j = 0; j < numPixels; j++ )
						{
							int p = indexed[j];

							if ( p >= palSize )
								p = 0;

							rgb[j * 3 + 0] = pal[p * 3 + 0];
							rgb[j * 3 + 1] = pal[p * 3 + 1];
							rgb[j * 3 + 2] = pal[p * 3 + 2];
						}

						const lbTexture_t *added = AddTexture( ref.m_name, (int)mip.width, (int)mip.height, rgb );

						if ( added )
						{
							textureOut = *added;
							ok = true;
						}

						free( rgb );
					}
				}
			}
		}
	}

	free( lump );
	fclose( f );

	return ok;
}

// bacontsu: indexes the lump table of a WAD3 file. No pixel data is read here
bool CTextureCache::AddFile( const char *filePath )
{
	FILE *f = fopen( filePath, "rb" );

	if ( !f )
		return false;

	lbWadInfo_t header;

	if ( fread( &header, 1, sizeof( header ), f ) != sizeof( header ) )
	{
		fclose( f );
		return false;
	}

	if ( header.identification != LB_IDWADHEADER || header.numlumps <= 0 )
	{
		fclose( f );
		return false;
	}

	if ( fseek( f, header.infotableofs, SEEK_SET ) != 0 )
	{
		fclose( f );
		return false;
	}

	char baseName[MAX_PATH] = { 0 };
	V_ExtractFileBase( filePath, baseName, sizeof( baseName ) );

	int indexed = 0;

	for ( int i = 0; i < header.numlumps; i++ )
	{
		if ( (int)m_lumps.size() >= LB_MAX_LUMPS )
			break;

		lbLumpInfo_t lump;

		if ( fread( &lump, 1, sizeof( lump ), f ) != sizeof( lump ) )
			break;

		if ( lump.type != LB_TYP_MIPTEX )
			continue;

		if ( lump.filepos < 0 || lump.disksize < (int)sizeof( lbMipTex_t ) )
			continue;

		char name[16];
		memcpy( name, lump.name, sizeof( name ) );
		name[sizeof( name ) - 1] = '\0';

		if ( !name[0] )
			continue;

		// first wad wins, same as the editor
		std::string key = LB_Lower( name );

		if ( m_lumpIndex.find( key ) != m_lumpIndex.end() )
			continue;

		LumpRef ref;
		memset( &ref.m_name, 0, sizeof( ref.m_name ) );
		strncpy( ref.m_name, name, sizeof( ref.m_name ) - 1 );
		ref.m_wadPath = filePath;
		ref.m_filePos = (long)lump.filepos;
		ref.m_diskSize = (long)lump.disksize;

		m_lumps.push_back( ref );
		m_lumpIndex[key] = (int)m_lumps.size() - 1;

		indexed++;
	}

	fclose( f );

	if ( indexed > 0 )
	{
		m_wadNames.push_back( baseName );
		LB_LogMsg( "wad: %s indexed %d textures", baseName, indexed );
	}

	return indexed > 0;
}

int CTextureCache::AddDirectory( const char *dir, bool recursive )
{
	if ( !dir || !dir[0] )
		return 0;

	std::error_code ec;

	std::filesystem::path root( dir );

	if ( !std::filesystem::exists( root, ec ) || !std::filesystem::is_directory( root, ec ) )
		return 0;

	int added = 0;

	auto tryAdd = [&]( const std::filesystem::directory_entry &entry )
	{
		if ( added >= 64 )
			return;

		if ( !entry.is_regular_file( ec ) )
			return;

		std::string ext = entry.path().extension().string();

		std::transform( ext.begin(), ext.end(), ext.begin(), []( unsigned char c ) { return (char)tolower( c ); } );

		if ( ext != ".wad" )
			return;

		if ( AddFile( entry.path().string().c_str() ) )
			added++;
	};

	try
	{
		if ( recursive )
		{
			// bounded depth: wads live in the mod directory, a deep walk can hang the editor
			const int kMaxDepth = 3;

			std::filesystem::recursive_directory_iterator it( root, std::filesystem::directory_options::skip_permission_denied, ec );
			std::filesystem::recursive_directory_iterator end;

			for ( ; it != end; it.increment( ec ) )
			{
				if ( ec || added >= 64 )
					break;

				if ( it.depth() >= kMaxDepth )
					it.disable_recursion_pending();

				tryAdd( *it );
			}
		}
		else
		{
			std::filesystem::directory_iterator it( root, std::filesystem::directory_options::skip_permission_denied, ec );
			std::filesystem::directory_iterator end;

			for ( ; it != end; it.increment( ec ) )
			{
				if ( ec )
					break;

				tryAdd( *it );
			}
		}
	}
	catch ( ... )
	{
		// never let a filesystem exception escape into the editor
	}

	return added;
}

const lbTexture_t *CTextureCache::Find( const char *name )
{
	if ( !name || !name[0] )
		return NULL;

	for ( size_t i = 0; i < m_textures.size(); i++ )
	{
		if ( !stricmp( m_textures[i]->m_name, name ) )
			return m_textures[i];
	}

	std::map<std::string, int>::const_iterator it = m_lumpIndex.find( LB_Lower( name ) );

	if ( it == m_lumpIndex.end() )
		return NULL;

	lbTexture_t texture;

	memset( &texture, 0, sizeof( texture ) );

	if ( !DecodeLump( it->second, texture ) )
		return NULL;

	return m_textures.empty() ? NULL : m_textures[m_textures.size() - 1];
}

int CTextureCache::GetCount() const
{
	return (int)m_textures.size();
}

int CTextureCache::GetIndexedCount() const
{
	return (int)m_lumps.size();
}

const char *CTextureCache::GetLoadedWadName( int index ) const
{
	if ( index < 0 || index >= (int)m_wadNames.size() )
		return "";

	return m_wadNames[index].c_str();
}

void CTextureCache::Purge()
{
	for ( size_t i = 0; i < m_textures.size(); i++ )
	{
		if ( m_textures[i] )
		{
			if ( m_textures[i]->m_pixels )
				free( m_textures[i]->m_pixels );

			delete m_textures[i];
		}
	}

	m_textures.clear();
	m_lumps.clear();
	m_lumpIndex.clear();
	m_wadNames.clear();

	m_decodedBytes = 0;
}
