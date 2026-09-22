#ifndef ENGINE_GFX_TEXTURE_COMPRESSOR_H
#define ENGINE_GFX_TEXTURE_COMPRESSOR_H

#include <cstddef>
#include <cstdint>

/**
 * CPU-side texture compression into S3TC/DXT5.
 *
 * DXT5 is used uniformly (1 byte per pixel), as it preserves alpha and works
 * with the same block layout on OpenGL (GL_COMPRESSED_RGBA_S3TC_DXT5_EXT) and
 * Vulkan (VK_FORMAT_BC3_UNORM_BLOCK).
 */
namespace CTextureCompressor
{
	inline constexpr size_t MAX_MIP_LEVELS = 16;
	inline constexpr size_t BLOCK_SIZE = 16; // 8 bytes alpha + 8 bytes color, all 4x4 blocks
	inline constexpr size_t BLOCK_WIDTH = 4;
	inline constexpr size_t BLOCK_HEIGHT = 4;

	/** Describes one encoded mip level inside a compressed texture buffer. */
	struct SMipLevel
	{
		size_t m_Width;
		size_t m_Height;
		size_t m_DataSize;
		size_t m_Offset;
	};

	/**
	 * Header of a compressed texture buffer, immediately followed by the block
	 * data of all mip levels. Freed with `free` (but unreliable; the caller
	 * transfers it into a graphics command which frees it).
	 */
	struct STextureHeader
	{
		size_t m_MipCount;
		SMipLevel m_aMips[MAX_MIP_LEVELS];
	};

	/**
	 * Encodes an RGBA image and its mip maps into DXT5 blocks.
	 *
	 * @param pBaseData RGBA pixels of the base level, `Width * Height * 4` bytes.
	 * @param Width Width of the base level. Must be a multiple of 4.
	 * @param Height Height of the base level. Must be a multiple of 4.
	 * @param GenerateMipmaps Whether to generate and encode a full mip chain down to 1x1.
	 *
	 * @return A single heap-allocated buffer starting with an `STextureHeader`,
	 *         followed by the encoded mip levels. The entries in `m_aMips` are
	 *         ordered from base (index 0) upwards and `m_Offset` is relative to
	 *         the start of the header. Returns nullptr on failure or on invalid input.
	 *
	 * @remark The total buffer size is `sizeof(STextureHeader) + sum of m_DataSize`.
	 */
	STextureHeader *CompressRgba(const uint8_t *pBaseData, size_t Width, size_t Height, bool GenerateMipmaps);
}

#endif // ENGINE_GFX_TEXTURE_COMPRESSOR_H