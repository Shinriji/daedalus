/*
Copyright (C) 2001 StrmnNrmn

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/

#include "Base/Daedalus.h"
#include "HLEGraphics/CachedTexture.h"

#include <vector>

#include "Base/MathUtil.h"
#include "Config/ConfigOptions.h"
#include "Core/ROM.h"
#include "Debug/DBGConsole.h"
#include "Debug/Dump.h"
#include "Graphics/ColourValue.h"
#include "Graphics/NativePixelFormat.h"
#include "Graphics/NativeTexture.h"
#include "Graphics/PngUtil.h"
#include "Graphics/TextureTransform.h"
#include "HLEGraphics/ConvertImage.h"
#include "HLEGraphics/ConvertTile.h"
#include "HLEGraphics/TextureInfo.h"
#include "Math/Math.h"
#include "Ultra/ultra_gbi.h"
#include "System/IO.h"
#include "Utility/Profiler.h"

static std::vector<u8>		gTexelBuffer;

static bool GenerateTexels(NativePf8888 ** p_texels, const TextureInfo & ti,
						   u32 pitch, u32 buffer_size)
{
	if( gTexelBuffer.size() < buffer_size )
	{
#ifdef DAEDALUS_DEBUG_DISPLAYLIST
		printf( "Resizing texel buffer to %d bytes. Texture is %dx%d\n", buffer_size, ti.GetWidth(), ti.GetHeight() );
#endif
		gTexelBuffer.resize( buffer_size );
	}

	NativePf8888 * texels = reinterpret_cast<NativePf8888*>(&gTexelBuffer[0]);

	// NB: if line is 0, it implies this is a direct load from ram (e.g. DLParser_Sprite2DDraw etc)
	// This check isn't robust enough, SSV set ti.Line == 0 in game without calling Sprite2D
	if (ti.GetLine() > 0)
	{
		if (!ConvertTile(ti, texels, pitch))
		{
			return false;
		}
	}
	else
	{
		if (!ConvertTexture(ti, texels, pitch))
		{
			return false;
		}
	}

	*p_texels = texels;
	return true;
}

static void UpdateTexture( const TextureInfo & ti, CNativeTexture * texture )
{
	DAEDALUS_PROFILE( "Texture Conversion" );

	DAEDALUS_ASSERT( texture != NULL, "No texture" );

	if ( texture != NULL && texture->HasData() )
	{
		u32 stride = texture->GetStride();

		NativePf8888 *	texels;
		if( GenerateTexels( &texels, ti, stride, texture->GetBytesRequired() ) )
		{
			//
			//	Recolour the texels
			//
			if( ti.GetWhite() )
			{
				Recolour( texels, ti.GetWidth(), ti.GetHeight(), stride, c32::White );
			}

			//
			//	Clamp edges. We do this so that non power-of-2 textures whose whose width/height
			//	is less than the mask value clamp correctly. It still doesn't fix those
			//	textures with a width which is greater than the power-of-2 size.
			//
			ClampTexels( texels, ti.GetWidth(), ti.GetHeight(), texture->GetCorrectedWidth(), texture->GetCorrectedHeight(), stride );

			//
			//	Mirror the texels if required (in-place)
			//
			bool mirror_s = ti.GetEmulateMirrorS();
			bool mirror_t = ti.GetEmulateMirrorT();
			if( mirror_s || mirror_t )
			{
				MirrorTexels( mirror_s, mirror_t, texels, stride, texels, stride, ti.GetWidth(), ti.GetHeight() );
			}

			texture->SetData( texels );
		}
	}
}

CachedTexture * CachedTexture::Create( const TextureInfo & ti )
{
	if( ti.GetWidth() == 0 || ti.GetHeight() == 0 )
	{
		DAEDALUS_ERROR( "Trying to create 0 width/height texture" );
		return NULL;
	}

	CachedTexture *	texture = new CachedTexture( ti );
	if (!texture->Initialise())
	{
		return NULL;
	}

	return texture;
}

CachedTexture::CachedTexture( const TextureInfo & ti )
:	mTextureInfo( ti )
,	mpTexture(NULL)
,	mFrameLastUpToDate( gRDPFrame )
,	mFrameLastUsed( gRDPFrame )
{
}

CachedTexture::~CachedTexture()
{
}

bool CachedTexture::Initialise()
{
	DAEDALUS_ASSERT_Q(mpTexture == NULL);

	u32 width  = mTextureInfo.GetWidth();
	u32 height = mTextureInfo.GetHeight();

	if (mTextureInfo.GetEmulateMirrorS()) width  *= 2;
	if (mTextureInfo.GetEmulateMirrorT()) height *= 2;

	mpTexture = CNativeTexture::Create( width, height );
	if( mpTexture != NULL )
	{
		// If this we're performing Texture updated checks, randomly offset the
		// 'FrameLastUpToDate' time. This ensures when lots of textures are
		// created on the same frame we update them over a nice distribution of frames.

		if(gCheckTextureHashFrequency > 0)
		{
			mFrameLastUpToDate = gRDPFrame + (FastRand() & (gCheckTextureHashFrequency - 1));
		}
		UpdateTexture( mTextureInfo, mpTexture );
	}

	return mpTexture != NULL;
}

void CachedTexture::UpdateIfNecessary()
{
	if( !IsFresh() )
	{
		UpdateTexture( mTextureInfo, mpTexture );

		// FIXME(strmnrmn): should probably recreate mpWhiteTexture if it exists, else it may have stale data.

		mFrameLastUpToDate = gRDPFrame;
	}

	mFrameLastUsed = gRDPFrame;
}

// Has this cached texture been updated recently?
bool CachedTexture::IsFresh() const
{
	return gRDPFrame == mFrameLastUsed;
}

bool CachedTexture::HasExpired() const
{
	//Otherwise we wait 20+random(0-3) frames before trashing the texture if unused
	//Spread trashing them over time so not all get killed at once (lower value uses less VRAM) //Corn
	return gRDPFrame - mFrameLastUsed > (20 + (FastRand() & 0x3));
}

#ifdef DAEDALUS_DEBUG_DISPLAYLIST
void CachedTexture::DumpTexture( const TextureInfo & ti, const CNativeTexture * texture )
{
	DAEDALUS_ASSERT(texture != NULL, "Should have a texture");

	if( texture != NULL && texture->HasData() )
	{
		char filename[256];
		sprintf( filename, "%08x-%s_%dbpp-%dx%d-%dx%d.png",
							ti.GetLoadAddress(), ti.GetFormatName(), ti.GetSizeInBits(),
							0, 0,		// Left/Top
							ti.GetWidth(), ti.GetHeight() );

		std::string dumpdir = IO::Path::Join(g_ROM.settings.GameName, "Textures");
		std::string filepath = IO::Path::Join(Dump_GetDumpDirectory(dumpdir), filename);

		NativePf8888 *	texels;

		// Note that we re-convert the texels because those in the native texture may well already
		// be swizzle. Maybe we should just have an unswizzle routine?
		if( GenerateTexels( &texels, ti, texture->GetStride(), texture->GetBytesRequired() ) )
		{
			// NB - this does not include the mirrored texels
			PngSaveImage( filepath, texels, texture->GetStride(), ti.GetWidth(), ti.GetHeight(), true );
		}
	}
}
#endif // DAEDALUS_DEBUG_DISPLAYLIST
