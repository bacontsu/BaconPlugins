//=====================================================================================//
// bacontsu: wad3 texture cache
//=====================================================================================//
#if !defined( LIGHTBAKE_WAD_H )
#define LIGHTBAKE_WAD_H

#include <stdio.h>
#include <string.h>
#include <vector>
#include <string>
#include <map>

#include "PluginMeta.h"

// WAD3 identification (little endian "WAD3")
#define LB_IDWADHEADER	( ( '3' << 24 ) + ( 'D' << 16 ) + ( 'A' << 8 ) + 'W' )

#define LB_TYP_MIPTEX	67

typedef struct lbWadInfo_s
{
	int identification;
	int numlumps;
	int infotableofs;
} lbWadInfo_t;

typedef struct lbLumpInfo_s
{
	int filepos;
	int disksize;
	int size;
	char type;
	char compression;
	char pad1, pad2;
	char name[16];
} lbLumpInfo_t;

typedef struct lbMipTex_s
{
	char name[16];
	unsigned int width;
	unsigned int height;
	unsigned int offsets[4];
} lbMipTex_t;

// bacontsu: a single decoded texture. m_pixels is always a tightly packed RGB24 buffer
struct lbTexture_t
{
	char m_name[16];
	int m_width;
	int m_height;
	unsigned char *m_pixels;
};

class CTextureCache
{
public:
	CTextureCache();
	~CTextureCache();

	// indexes every *.wad found in "dir" (bounded depth, bounded count)
	int AddDirectory( const char *dir, bool recursive );

	// indexes a single wad file
	bool AddFile( const char *filePath );

	// finds a texture by name (case insensitive), decoding it on first use
	const lbTexture_t *Find( const char *name );

	int GetCount() const;
	int GetIndexedCount() const;
	const char *GetLoadedWadName( int index ) const;

	void Purge();

private:
	bool DecodeLump( int lumpIndex, lbTexture_t &textureOut );
	const lbTexture_t *AddTexture( const char *name, int width, int height, const unsigned char *rgb );

	struct LumpRef
	{
		char m_name[16];
		std::string m_wadPath;
		long m_filePos;
		long m_diskSize;
	};

	std::vector<LumpRef> m_lumps;
	std::map<std::string, int> m_lumpIndex;

	// heap allocated on purpose: callers keep lbTexture_t pointers around and the
	// vector must never move the objects they point into
	std::vector<lbTexture_t *> m_textures;
	std::vector<std::string> m_wadNames;

	// decoded bytes, used to stay inside a sane memory budget
	size_t m_decodedBytes;

	bool m_haveSeenWad;
};

#endif // !LIGHTBAKE_WAD_H
