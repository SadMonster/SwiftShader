// SwiftShader Software Renderer
//
// Copyright(c) 2005-2013 TransGaming Inc.
//
// All rights reserved. No part of this software may be copied, distributed, transmitted,
// transcribed, stored in a retrieval system, translated into any human or computer
// language by any means, or disclosed to third parties without the explicit written
// agreement of TransGaming Inc. Without such an agreement, no rights or licenses, express
// or implied, including but not limited to any patent rights, are granted to you.
//

#include "Image.hpp"

#include "Texture.h"
#include "utilities.h"
#include "../common/debug.h"
#include "Common/Thread.hpp"

#include <GLES2/gl2ext.h>

namespace gl
{
	static sw::Resource *getParentResource(Texture *texture)
	{
		if(texture)
		{
			return texture->getResource();
		}

		return 0;
	}

	Image::Image(Texture *parentTexture, GLsizei width, GLsizei height, GLenum format, GLenum type)
		: parentTexture(parentTexture)
		, egl::Image(getParentResource(parentTexture), width, height, format, type, selectInternalFormat(format, type))
	{
		referenceCount = 1;
	}

	Image::Image(Texture *parentTexture, GLsizei width, GLsizei height, sw::Format internalFormat, int multiSampleDepth, bool lockable, bool renderTarget)
		: parentTexture(parentTexture)
		, egl::Image(getParentResource(parentTexture), width, height, multiSampleDepth, internalFormat, lockable, renderTarget)
	{
		referenceCount = 1;
	}

	Image::~Image()
	{
		ASSERT(referenceCount == 0);
	}

	void Image::addRef()
	{
		if(parentTexture)
		{
			return parentTexture->addRef();
		}

		sw::atomicIncrement(&referenceCount);
	}

	void Image::release()
	{
		if(parentTexture)
		{
			return parentTexture->release();
		}

		if(referenceCount > 0)
		{
			sw::atomicDecrement(&referenceCount);
		}

		if(referenceCount == 0)
		{
			ASSERT(!shared);   // Should still hold a reference if eglDestroyImage hasn't been called
			delete this;
		}
	}

	void Image::unbind(const egl::Texture *parent)
	{
		if(parentTexture == parent)
		{
			parentTexture = 0;
		}

		release();
	}

	sw::Format Image::selectInternalFormat(GLenum format, GLenum type)
	{
		if(format == GL_ETC1_RGB8_OES)
		{
			return sw::FORMAT_ETC1;
		}
		else
		#if S3TC_SUPPORT
		if(format == GL_COMPRESSED_RGB_S3TC_DXT1_EXT ||
		   format == GL_COMPRESSED_RGBA_S3TC_DXT1_EXT)
		{
			return sw::FORMAT_DXT1;
		}
		else if(format == GL_COMPRESSED_RGBA_S3TC_DXT3_ANGLE)
		{
			return sw::FORMAT_DXT3;
		}
		else if(format == GL_COMPRESSED_RGBA_S3TC_DXT5_ANGLE)
		{
			return sw::FORMAT_DXT5;
		}
		else
		#endif
		if(type == GL_FLOAT)
		{
			return sw::FORMAT_A32B32G32R32F;
		}
		else if(type == GL_HALF_FLOAT_OES)
		{
			return sw::FORMAT_A16B16G16R16F;
		}
		else if(type == GL_UNSIGNED_BYTE)
		{
			if(format == GL_LUMINANCE)
			{
				return sw::FORMAT_L8;
			}
			else if(format == GL_LUMINANCE_ALPHA)
			{
				return sw::FORMAT_A8L8;
			}
			else if(format == GL_RGBA || format == GL_BGRA_EXT)
			{
				return sw::FORMAT_A8R8G8B8;
			}
			else if(format == GL_RGB)
			{
				return sw::FORMAT_X8R8G8B8;
			}
			else if(format == GL_ALPHA)
			{
				return sw::FORMAT_A8;
			}
			else UNREACHABLE();
		}
		else if(type == GL_UNSIGNED_SHORT || type == GL_UNSIGNED_INT)
		{
			if(format == GL_DEPTH_COMPONENT)
			{
				return sw::FORMAT_D32FS8_TEXTURE;
			}
			else UNREACHABLE();
		}
		else if(type == GL_UNSIGNED_INT_24_8_OES)
		{
			if(format == GL_DEPTH_STENCIL_OES)
			{
				return sw::FORMAT_D32FS8_TEXTURE;
			}
			else UNREACHABLE();
		}
		else if(type == GL_UNSIGNED_SHORT_4_4_4_4)
		{
			return sw::FORMAT_A8R8G8B8;
		}
		else if(type == GL_UNSIGNED_SHORT_5_5_5_1)
		{
			return sw::FORMAT_A8R8G8B8;
		}
		else if(type == GL_UNSIGNED_SHORT_5_6_5)
		{
			return sw::FORMAT_X8R8G8B8;
		}
		else UNREACHABLE();

		return sw::FORMAT_A8R8G8B8;
	}

	void Image::loadImageData(GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, GLint unpackAlignment, const void *input)
	{
		GLsizei inputPitch = ComputePitch(width, format, type, unpackAlignment);
		void *buffer = lock(0, 0, sw::LOCK_WRITEONLY);
		
		if(buffer)
		{
			switch(type)
			{
			case GL_UNSIGNED_BYTE:
				switch(format)
				{
				case GL_ALPHA:
					loadAlphaImageData(xoffset, yoffset, width, height, inputPitch, input, buffer);
					break;
				case GL_LUMINANCE:
					loadLuminanceImageData(xoffset, yoffset, width, height, inputPitch, input, buffer);
					break;
				case GL_LUMINANCE_ALPHA:
					loadLuminanceAlphaImageData(xoffset, yoffset, width, height, inputPitch, input, buffer);
					break;
				case GL_RGB:
					loadRGBUByteImageData(xoffset, yoffset, width, height, inputPitch, input, buffer);
					break;
				case GL_RGBA:
					loadRGBAUByteImageData(xoffset, yoffset, width, height, inputPitch, input, buffer);
					break;
				case GL_BGRA_EXT:
					loadBGRAImageData(xoffset, yoffset, width, height, inputPitch, input, buffer);
					break;
				default: UNREACHABLE();
				}
				break;
			case GL_UNSIGNED_SHORT_5_6_5:
				switch(format)
				{
				case GL_RGB:
					loadRGB565ImageData(xoffset, yoffset, width, height, inputPitch, input, buffer);
					break;
				default: UNREACHABLE();
				}
				break;
			case GL_UNSIGNED_SHORT_4_4_4_4:
				switch(format)
				{
				case GL_RGBA:
					loadRGBA4444ImageData(xoffset, yoffset, width, height, inputPitch, input, buffer);
					break;
				default: UNREACHABLE();
				}
				break;
			case GL_UNSIGNED_SHORT_5_5_5_1:
				switch(format)
				{
				case GL_RGBA:
					loadRGBA5551ImageData(xoffset, yoffset, width, height, inputPitch, input, buffer);
					break;
				default: UNREACHABLE();
				}
				break;
			case GL_FLOAT:
				switch(format)
				{
				// float textures are converted to RGBA, not BGRA
				case GL_ALPHA:
					loadAlphaFloatImageData(xoffset, yoffset, width, height, inputPitch, input, buffer);
					break;
				case GL_LUMINANCE:
					loadLuminanceFloatImageData(xoffset, yoffset, width, height, inputPitch, input, buffer);
					break;
				case GL_LUMINANCE_ALPHA:
					loadLuminanceAlphaFloatImageData(xoffset, yoffset, width, height, inputPitch, input, buffer);
					break;
				case GL_RGB:
					loadRGBFloatImageData(xoffset, yoffset, width, height, inputPitch, input, buffer);
					break;
				case GL_RGBA:
					loadRGBAFloatImageData(xoffset, yoffset, width, height, inputPitch, input, buffer);
					break;
				default: UNREACHABLE();
				}
				break;
			  case GL_HALF_FLOAT_OES:
				switch(format)
				{
				// float textures are converted to RGBA, not BGRA
				case GL_ALPHA:
					loadAlphaHalfFloatImageData(xoffset, yoffset, width, height, inputPitch, input, buffer);
					break;
				case GL_LUMINANCE:
					loadLuminanceHalfFloatImageData(xoffset, yoffset, width, height, inputPitch, input, buffer);
					break;
				case GL_LUMINANCE_ALPHA:
					loadLuminanceAlphaHalfFloatImageData(xoffset, yoffset, width, height, inputPitch, input, buffer);
					break;
				case GL_RGB:
					loadRGBHalfFloatImageData(xoffset, yoffset, width, height, inputPitch, input, buffer);
					break;
				case GL_RGBA:
					loadRGBAHalfFloatImageData(xoffset, yoffset, width, height, inputPitch, input, buffer);
					break;
				default: UNREACHABLE();
				}
				break;
			case GL_UNSIGNED_SHORT:
				loadD16ImageData(xoffset, yoffset, width, height, inputPitch, input, buffer);
				break;
			case GL_UNSIGNED_INT:
				loadD32ImageData(xoffset, yoffset, width, height, inputPitch, input, buffer);
				break;
			case GL_UNSIGNED_INT_24_8_OES:
				loadD24S8ImageData(xoffset, yoffset, width, height, inputPitch, input, buffer);
				break;
			default: UNREACHABLE();
			}
		}

		unlock();
	}

	void Image::loadAlphaImageData(GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, int inputPitch, const void *input, void *buffer) const
	{
		for(int y = 0; y < height; y++)
		{
			const unsigned char *source = static_cast<const unsigned char*>(input) + y * inputPitch;
			unsigned char *dest = static_cast<unsigned char*>(buffer) + (y + yoffset) * getPitch() + xoffset;

			memcpy(dest, source, width);
		}
	}

	void Image::loadAlphaFloatImageData(GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, int inputPitch, const void *input, void *buffer) const
	{
		for(int y = 0; y < height; y++)
		{
			const float *source = reinterpret_cast<const float*>(static_cast<const unsigned char*>(input) + y * inputPitch);
			float *dest = reinterpret_cast<float*>(static_cast<unsigned char*>(buffer) + (y + yoffset) * getPitch() + xoffset * 16);
			
			for(int x = 0; x < width; x++)
			{
				dest[4 * x + 0] = 0;
				dest[4 * x + 1] = 0;
				dest[4 * x + 2] = 0;
				dest[4 * x + 3] = source[x];
			}
		}
	}

	void Image::loadAlphaHalfFloatImageData(GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, int inputPitch, const void *input, void *buffer) const
	{
		for(int y = 0; y < height; y++)
		{
			const unsigned short *source = reinterpret_cast<const unsigned short*>(static_cast<const unsigned char*>(input) + y * inputPitch);
			unsigned short *dest = reinterpret_cast<unsigned short*>(static_cast<unsigned char*>(buffer) + (y + yoffset) * getPitch() + xoffset * 8);
			
			for(int x = 0; x < width; x++)
			{
				dest[4 * x + 0] = 0;
				dest[4 * x + 1] = 0;
				dest[4 * x + 2] = 0;
				dest[4 * x + 3] = source[x];
			}
		}
	}

	void Image::loadLuminanceImageData(GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, int inputPitch, const void *input, void *buffer) const
	{
		for(int y = 0; y < height; y++)
		{
			const unsigned char *source = static_cast<const unsigned char*>(input) + y * inputPitch;
			unsigned char *dest = static_cast<unsigned char*>(buffer) + (y + yoffset) * getPitch() + xoffset;

			memcpy(dest, source, width);
		}
	}

	void Image::loadLuminanceFloatImageData(GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, int inputPitch, const void *input, void *buffer) const
	{
		for(int y = 0; y < height; y++)
		{
			const float *source = reinterpret_cast<const float*>(static_cast<const unsigned char*>(input) + y * inputPitch);
			float *dest = reinterpret_cast<float*>(static_cast<unsigned char*>(buffer) + (y + yoffset) * getPitch() + xoffset * 16);
			
			for(int x = 0; x < width; x++)
			{
				dest[4 * x + 0] = source[x];
				dest[4 * x + 1] = source[x];
				dest[4 * x + 2] = source[x];
				dest[4 * x + 3] = 1.0f;
			}
		}
	}

	void Image::loadLuminanceHalfFloatImageData(GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, int inputPitch, const void *input, void *buffer) const
	{
		for(int y = 0; y < height; y++)
		{
			const unsigned short *source = reinterpret_cast<const unsigned short*>(static_cast<const unsigned char*>(input) + y * inputPitch);
			unsigned short *dest = reinterpret_cast<unsigned short*>(static_cast<unsigned char*>(buffer) + (y + yoffset) * getPitch() + xoffset * 8);
			
			for(int x = 0; x < width; x++)
			{
				dest[4 * x + 0] = source[x];
				dest[4 * x + 1] = source[x];
				dest[4 * x + 2] = source[x];
				dest[4 * x + 3] = 0x3C00; // SEEEEEMMMMMMMMMM, S = 0, E = 15, M = 0: 16bit flpt representation of 1
			}
		}
	}

	void Image::loadLuminanceAlphaImageData(GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, int inputPitch, const void *input, void *buffer) const
	{
		for(int y = 0; y < height; y++)
		{
			const unsigned char *source = static_cast<const unsigned char*>(input) + y * inputPitch;
			unsigned char *dest = static_cast<unsigned char*>(buffer) + (y + yoffset) * getPitch() + xoffset * 2;
        
			memcpy(dest, source, width * 2);
		}
	}

	void Image::loadLuminanceAlphaFloatImageData(GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, int inputPitch, const void *input, void *buffer) const
	{
		for(int y = 0; y < height; y++)
		{
			const float *source = reinterpret_cast<const float*>(static_cast<const unsigned char*>(input) + y * inputPitch);
			float *dest = reinterpret_cast<float*>(static_cast<unsigned char*>(buffer) + (y + yoffset) * getPitch() + xoffset * 16);
			
			for(int x = 0; x < width; x++)
			{
				dest[4 * x + 0] = source[2*x+0];
				dest[4 * x + 1] = source[2*x+0];
				dest[4 * x + 2] = source[2*x+0];
				dest[4 * x + 3] = source[2*x+1];
			}
		}
	}

	void Image::loadLuminanceAlphaHalfFloatImageData(GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, int inputPitch, const void *input, void *buffer) const
	{
		for(int y = 0; y < height; y++)
		{
			const unsigned short *source = reinterpret_cast<const unsigned short*>(static_cast<const unsigned char*>(input) + y * inputPitch);
			unsigned short *dest = reinterpret_cast<unsigned short*>(static_cast<unsigned char*>(buffer) + (y + yoffset) * getPitch() + xoffset * 8);
			
			for(int x = 0; x < width; x++)
			{
				dest[4 * x + 0] = source[2*x+0];
				dest[4 * x + 1] = source[2*x+0];
				dest[4 * x + 2] = source[2*x+0];
				dest[4 * x + 3] = source[2*x+1];
			}
		}
	}

	void Image::loadRGBUByteImageData(GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, int inputPitch, const void *input, void *buffer) const
	{
		for(int y = 0; y < height; y++)
		{
			const unsigned char *source = static_cast<const unsigned char*>(input) + y * inputPitch;
			unsigned char *dest = static_cast<unsigned char*>(buffer) + (y + yoffset) * getPitch() + xoffset * 4;
			
			for(int x = 0; x < width; x++)
			{
				dest[4 * x + 0] = source[x * 3 + 2];
				dest[4 * x + 1] = source[x * 3 + 1];
				dest[4 * x + 2] = source[x * 3 + 0];
				dest[4 * x + 3] = 0xFF;
			}
		}
	}

	void Image::loadRGB565ImageData(GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, int inputPitch, const void *input, void *buffer) const
	{
		for(int y = 0; y < height; y++)
		{
			const unsigned short *source = reinterpret_cast<const unsigned short*>(static_cast<const unsigned char*>(input) + y * inputPitch);
			unsigned char *dest = static_cast<unsigned char*>(buffer) + (y + yoffset) * getPitch() + xoffset * 4;
			
			for(int x = 0; x < width; x++)
			{
				unsigned short rgba = source[x];
				dest[4 * x + 0] = ((rgba & 0x001F) << 3) | ((rgba & 0x001F) >> 2);
				dest[4 * x + 1] = ((rgba & 0x07E0) >> 3) | ((rgba & 0x07E0) >> 9);
				dest[4 * x + 2] = ((rgba & 0xF800) >> 8) | ((rgba & 0xF800) >> 13);
				dest[4 * x + 3] = 0xFF;
			}
		}
	}

	void Image::loadRGBFloatImageData(GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, int inputPitch, const void *input, void *buffer) const
	{
		for(int y = 0; y < height; y++)
		{
			const float *source = reinterpret_cast<const float*>(static_cast<const unsigned char*>(input) + y * inputPitch);
			float *dest = reinterpret_cast<float*>(static_cast<unsigned char*>(buffer) + (y + yoffset) * getPitch() + xoffset * 16);
			
			for(int x = 0; x < width; x++)
			{
				dest[4 * x + 0] = source[x * 3 + 0];
				dest[4 * x + 1] = source[x * 3 + 1];
				dest[4 * x + 2] = source[x * 3 + 2];
				dest[4 * x + 3] = 1.0f;
			}
		}
	}

	void Image::loadRGBHalfFloatImageData(GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, int inputPitch, const void *input, void *buffer) const
	{
		for(int y = 0; y < height; y++)
		{
			const unsigned short *source = reinterpret_cast<const unsigned short*>(static_cast<const unsigned char*>(input) + y * inputPitch);
			unsigned short *dest = reinterpret_cast<unsigned short*>(static_cast<unsigned char*>(buffer) + (y + yoffset) * getPitch() + xoffset * 8);
			
			for(int x = 0; x < width; x++)
			{
				dest[4 * x + 0] = source[x * 3 + 0];
				dest[4 * x + 1] = source[x * 3 + 1];
				dest[4 * x + 2] = source[x * 3 + 2];
				dest[4 * x + 3] = 0x3C00; // SEEEEEMMMMMMMMMM, S = 0, E = 15, M = 0: 16bit flpt representation of 1
			}
		}
	}

	void Image::loadRGBAUByteImageData(GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, int inputPitch, const void *input, void *buffer) const
	{
		for(int y = 0; y < height; y++)
		{
			const unsigned int *source = reinterpret_cast<const unsigned int*>(static_cast<const unsigned char*>(input) + y * inputPitch);
			unsigned int *dest = reinterpret_cast<unsigned int*>(static_cast<unsigned char*>(buffer) + (y + yoffset) * getPitch() + xoffset * 4);

			for(int x = 0; x < width; x++)
			{
				unsigned int rgba = source[x];
				dest[x] = (rgba & 0xFF00FF00) | ((rgba << 16) & 0x00FF0000) | ((rgba >> 16) & 0x000000FF);
			}
		}
	}

	void Image::loadRGBA4444ImageData(GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, int inputPitch, const void *input, void *buffer) const
	{
		for(int y = 0; y < height; y++)
		{
			const unsigned short *source = reinterpret_cast<const unsigned short*>(static_cast<const unsigned char*>(input) + y * inputPitch);
			unsigned char *dest = static_cast<unsigned char*>(buffer) + (y + yoffset) * getPitch() + xoffset * 4;
			
			for(int x = 0; x < width; x++)
			{
				unsigned short rgba = source[x];
				dest[4 * x + 0] = ((rgba & 0x00F0) << 0) | ((rgba & 0x00F0) >> 4);
				dest[4 * x + 1] = ((rgba & 0x0F00) >> 4) | ((rgba & 0x0F00) >> 8);
				dest[4 * x + 2] = ((rgba & 0xF000) >> 8) | ((rgba & 0xF000) >> 12);
				dest[4 * x + 3] = ((rgba & 0x000F) << 4) | ((rgba & 0x000F) >> 0);
			}
		}
	}

	void Image::loadRGBA5551ImageData(GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, int inputPitch, const void *input, void *buffer) const
	{
		for(int y = 0; y < height; y++)
		{
			const unsigned short *source = reinterpret_cast<const unsigned short*>(static_cast<const unsigned char*>(input) + y * inputPitch);
			unsigned char *dest = static_cast<unsigned char*>(buffer) + (y + yoffset) * getPitch() + xoffset * 4;
			
			for(int x = 0; x < width; x++)
			{
				unsigned short rgba = source[x];
				dest[4 * x + 0] = ((rgba & 0x003E) << 2) | ((rgba & 0x003E) >> 3);
				dest[4 * x + 1] = ((rgba & 0x07C0) >> 3) | ((rgba & 0x07C0) >> 8);
				dest[4 * x + 2] = ((rgba & 0xF800) >> 8) | ((rgba & 0xF800) >> 13);
				dest[4 * x + 3] = (rgba & 0x0001) ? 0xFF : 0;
			}
		}
	}

	void Image::loadRGBAFloatImageData(GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, int inputPitch, const void *input, void *buffer) const
	{
		for(int y = 0; y < height; y++)
		{
			const float *source = reinterpret_cast<const float*>(static_cast<const unsigned char*>(input) + y * inputPitch);
			float *dest = reinterpret_cast<float*>(static_cast<unsigned char*>(buffer) + (y + yoffset) * getPitch() + xoffset * 16);
			
			memcpy(dest, source, width * 16);
		}
	}

	void Image::loadRGBAHalfFloatImageData(GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, int inputPitch, const void *input, void *buffer) const
	{
		for(int y = 0; y < height; y++)
		{
			const unsigned char *source = static_cast<const unsigned char*>(input) + y * inputPitch;
			unsigned char *dest = static_cast<unsigned char*>(buffer) + (y + yoffset) * getPitch() + xoffset * 8;
			
			memcpy(dest, source, width * 8);
		}
	}

	void Image::loadBGRAImageData(GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, int inputPitch, const void *input, void *buffer) const
	{
		for(int y = 0; y < height; y++)
		{
			const unsigned char *source = static_cast<const unsigned char*>(input) + y * inputPitch;
			unsigned char *dest = static_cast<unsigned char*>(buffer) + (y + yoffset) * getPitch() + xoffset * 4;
			
			memcpy(dest, source, width*4);
		}
	}

	void Image::loadD16ImageData(GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, int inputPitch, const void *input, void *buffer) const
	{
		for(int y = 0; y < height; y++)
		{
			const unsigned short *source = reinterpret_cast<const unsigned short*>(static_cast<const unsigned char*>(input) + y * inputPitch);
			float *dest = reinterpret_cast<float*>(static_cast<unsigned char*>(buffer) + (y + yoffset) * getPitch() + xoffset * 4);

			for(int x = 0; x < width; x++)
			{
				dest[x] = (float)source[x] / 0xFFFF;
			}
		}
	}

	void Image::loadD32ImageData(GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, int inputPitch, const void *input, void *buffer) const
	{
		for(int y = 0; y < height; y++)
		{
			const unsigned int *source = reinterpret_cast<const unsigned int*>(static_cast<const unsigned char*>(input) + y * inputPitch);
			float *dest = reinterpret_cast<float*>(static_cast<unsigned char*>(buffer) + (y + yoffset) * getPitch() + xoffset * 4);

			for(int x = 0; x < width; x++)
			{
				dest[x] = (float)source[x] / 0xFFFFFFFF;
			}
		}
	}

	void Image::loadD24S8ImageData(GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, int inputPitch, const void *input, void *buffer)
	{
		for(int y = 0; y < height; y++)
		{
			const unsigned int *source = reinterpret_cast<const unsigned int*>(static_cast<const unsigned char*>(input) + y * inputPitch);
			float *dest = reinterpret_cast<float*>(static_cast<unsigned char*>(buffer) + (y + yoffset) * getPitch() + xoffset * 4);

			for(int x = 0; x < width; x++)
			{
				dest[x] = (float)(source[x] & 0xFFFFFF00) / 0xFFFFFF00;
			}
		}

		unsigned char *stencil = reinterpret_cast<unsigned char*>(lockStencil(0, sw::PUBLIC));

		if(stencil)
		{
			for(int y = 0; y < height; y++)
			{
				const unsigned int *source = reinterpret_cast<const unsigned int*>(static_cast<const unsigned char*>(input) + y * inputPitch);
				unsigned char *dest = static_cast<unsigned char*>(stencil) + (y + yoffset) * getStencilPitchB() + xoffset;

				for(int x = 0; x < width; x++)
				{
					dest[x] = static_cast<unsigned char>(source[x] & 0x000000FF);   // FIXME: Quad layout
				}
			}

			unlockStencil();
		}
	}

	void Image::loadCompressedData(GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLsizei imageSize, const void *pixels)
	{
		int inputPitch = ComputeCompressedPitch(width, format);
		int rows = imageSize / inputPitch;
		void *buffer = lock(xoffset, yoffset, sw::LOCK_WRITEONLY);

        if(buffer)
        {
			for(int i = 0; i < rows; i++)
			{
				memcpy((void*)((GLbyte*)buffer + i * getPitch()), (void*)((GLbyte*)pixels + i * inputPitch), inputPitch);
			}
        }

		unlock();
	}
}