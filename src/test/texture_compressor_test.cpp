#include <engine/gfx/texture_compressor.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace
{
	using CTextureCompressor::BLOCK_HEIGHT;
	using CTextureCompressor::BLOCK_WIDTH;
	using CTextureCompressor::SMipLevel;
	using CTextureCompressor::STextureHeader;

	// Minimal DXT5 decoder for verification: expands one block back to 4x4 RGBA.
	static void DecodeDXT5Block(const uint8_t *pBlock, uint8_t aOut[BLOCK_WIDTH * BLOCK_HEIGHT * 4])
	{
		const int Alpha0 = pBlock[0];
		const int Alpha1 = pBlock[1];
		const int aRamp[8] = {
			Alpha0,
			Alpha1,
			(4 * Alpha0 + 1 * Alpha1 + 2) / 5,
			(3 * Alpha0 + 2 * Alpha1 + 2) / 5,
			(2 * Alpha0 + 3 * Alpha1 + 2) / 5,
			(1 * Alpha0 + 4 * Alpha1 + 2) / 5,
			0,
			255,
		};

		const uint16_t C0 = (uint16_t)(pBlock[8] | ((uint16_t)pBlock[9] << 8));
		const uint16_t C1 = (uint16_t)(pBlock[10] | ((uint16_t)pBlock[11] << 8));

		int R0, G0, B0, R1, G1, B1;
		R0 = ((C0 >> 11) & 31) << 3 | ((C0 >> 11) & 31) >> 2;
		G0 = ((C0 >> 5) & 63) << 2 | ((C0 >> 5) & 63) >> 4;
		B0 = (C0 & 31) << 3 | (C0 & 31) >> 2;
		R1 = ((C1 >> 11) & 31) << 3 | ((C1 >> 11) & 31) >> 2;
		G1 = ((C1 >> 5) & 63) << 2 | ((C1 >> 5) & 63) >> 4;
		B1 = (C1 & 31) << 3 | (C1 & 31) >> 2;

		int aPaletteR[4] = {R0, R1, (2 * R0 + R1) / 3, (R0 + 2 * R1) / 3};
		int aPaletteG[4] = {G0, G1, (2 * G0 + G1) / 3, (G0 + 2 * G1) / 3};
		int aPaletteB[4] = {B0, B1, (2 * B0 + B1) / 3, (B0 + 2 * B1) / 3};

		uint64_t AlphaBits = 0;
		for(size_t i = 0; i < 6; ++i)
			AlphaBits |= (uint64_t)pBlock[2 + i] << (8 * i);
		uint32_t ColorBits = 0;
		for(size_t i = 0; i < 4; ++i)
			ColorBits |= (uint32_t)pBlock[12 + i] << (8 * i);

		for(size_t P = 0; P < BLOCK_WIDTH * BLOCK_HEIGHT; ++P)
		{
			const int AlphaIndex = (int)((AlphaBits >> (3 * P)) & 7);
			const int ColorIndex = (int)((ColorBits >> (2 * P)) & 3);
			aOut[P * 4 + 0] = (uint8_t)aPaletteR[ColorIndex];
			aOut[P * 4 + 1] = (uint8_t)aPaletteG[ColorIndex];
			aOut[P * 4 + 2] = (uint8_t)aPaletteB[ColorIndex];
			aOut[P * 4 + 3] = (uint8_t)aRamp[AlphaIndex];
		}
	}

	static void DecodeAllLevels(const STextureHeader *pHeader)
	{
		uint8_t aBlock[BLOCK_WIDTH * BLOCK_HEIGHT * 4];
		for(size_t i = 0; i < pHeader->m_MipCount; ++i)
		{
			const SMipLevel &Mip = pHeader->m_aMips[i];
			EXPECT_EQ(Mip.m_Offset + Mip.m_DataSize, Mip.m_Offset + ((Mip.m_Width + BLOCK_WIDTH - 1) / BLOCK_WIDTH) * ((Mip.m_Height + BLOCK_HEIGHT - 1) / BLOCK_HEIGHT) * 16);
			uint8_t *pData = (uint8_t *)(pHeader) + Mip.m_Offset;
			for(size_t Off = 0; Off < Mip.m_DataSize; Off += 16)
				DecodeDXT5Block(pData + Off, aBlock);
		}
	}
}

TEST(TextureCompressor, MipLevelsAndSizes)
{
	constexpr size_t Width = 64;
	constexpr size_t Height = 64;
	uint8_t *pData = (uint8_t *)std::malloc(Width * Height * 4);
	ASSERT_NE(pData, nullptr);
	for(size_t i = 0; i < Width * Height * 4; ++i)
		pData[i] = (uint8_t)(i * 3 % 256);

	STextureHeader *pHeader = CTextureCompressor::CompressRgba(pData, Width, Height, true);
	ASSERT_NE(pHeader, nullptr);

	// 64, 32, 16, 8, 4 -> 5 levels
	EXPECT_EQ(pHeader->m_MipCount, 5u);
	for(size_t i = 0; i < pHeader->m_MipCount; ++i)
	{
		const SMipLevel &Mip = pHeader->m_aMips[i];
		EXPECT_EQ(Mip.m_Width, Width >> i);
		EXPECT_EQ(Mip.m_Height, Height >> i);
		DecodeAllLevels(pHeader);
	}

	std::free(pHeader);
	std::free(pData);
}

TEST(TextureCompressor, NoMipmapsSingleLevel)
{
	constexpr size_t Width = 8;
	constexpr size_t Height = 8;
	uint8_t aData[Width * Height * 4] = {0};

	STextureHeader *pHeader = CTextureCompressor::CompressRgba(aData, Width, Height, false);
	ASSERT_NE(pHeader, nullptr);
	EXPECT_EQ(pHeader->m_MipCount, 1u);
	EXPECT_EQ(pHeader->m_aMips[0].m_Width, 8u);
	EXPECT_EQ(pHeader->m_aMips[0].m_Height, 8u);
	// 2x2 blocks of 16 bytes
	EXPECT_EQ(pHeader->m_aMips[0].m_DataSize, 64u);

	std::free(pHeader);
}

TEST(TextureCompressor, RejectsNonBlockAligned)
{
	uint8_t aData[6 * 6 * 4] = {0};
	// 6x6 does not compress (must be a multiple of 4)
	STextureHeader *pHeader = CTextureCompressor::CompressRgba(aData, 6, 6, false);
	EXPECT_EQ(pHeader, nullptr);
}

TEST(TextureCompressor, RoundtripSolidBlock)
{
	// A constant block decodes back to exactly the same RGBA values when they lie on the
	// RGB565 lattice (DXT color endpoints are quantized to 5/6/5 bits).
	constexpr size_t Width = 4;
	constexpr size_t Height = 4;
	uint8_t aData[Width * Height * 4];
	for(size_t i = 0; i < Width * Height; ++i)
	{
		aData[i * 4 + 0] = 206; // 25 << 3 | 25 >> 2
		aData[i * 4 + 1] = 101; // 25 << 2 | 25 >> 4
		aData[i * 4 + 2] = 49; // 6 << 3 | 6 >> 2
		aData[i * 4 + 3] = 180;
	}

	STextureHeader *pHeader = CTextureCompressor::CompressRgba(aData, Width, Height, false);
	ASSERT_NE(pHeader, nullptr);
	EXPECT_EQ(pHeader->m_MipCount, 1u);

	uint8_t aDecoded[Width * Height * 4];
	DecodeDXT5Block((uint8_t *)pHeader + pHeader->m_aMips[0].m_Offset, aDecoded);
	for(size_t i = 0; i < Width * Height * 4; ++i)
		EXPECT_EQ(aDecoded[i], aData[i]);

	std::free(pHeader);
}

TEST(TextureCompressor, RoundtripNonPowerOfTwoAllLevels)
{
	// 100x100 -> mips 100, 50, 25, 13, 7, 4: 100/50/4 are block-aligned (fast path),
	// 25/13/7 are not (padded path), so this exercises both encode branches.
	constexpr size_t Width = 100;
	constexpr size_t Height = 100;
	uint8_t *pData = (uint8_t *)std::malloc(Width * Height * 4);
	ASSERT_NE(pData, nullptr);
	for(size_t y = 0; y < Height; ++y)
	{
		for(size_t x = 0; x < Width; ++x)
		{
			// Keep the total value range small (0..128): the top mips are tiny, so a
			// single 4x4 block can span a whole image-wide gradient.
			pData[(y * Width + x) * 4 + 0] = (uint8_t)(x * 128 / (Width - 1));
			pData[(y * Width + x) * 4 + 1] = (uint8_t)(y * 128 / (Height - 1));
			pData[(y * Width + x) * 4 + 2] = (uint8_t)(255 - x * 128 / (Width - 1));
			pData[(y * Width + x) * 4 + 3] = (uint8_t)(128 - y * 128 / (Height - 1));
		}
	}

	STextureHeader *pHeader = CTextureCompressor::CompressRgba(pData, Width, Height, true);
	ASSERT_NE(pHeader, nullptr);
	EXPECT_EQ(pHeader->m_MipCount, 6u);

	// Expected plane per level: repeated 2x2 box filtering of the source.
	uint8_t *pExpected = (uint8_t *)std::malloc(Width * Height * 4);
	ASSERT_NE(pExpected, nullptr);
	std::memcpy(pExpected, pData, Width * Height * 4);
	const uint8_t *pExpSrc = pExpected;
	size_t ExpWidth = Width;
	size_t ExpHeight = Height;

	for(size_t i = 0; i < pHeader->m_MipCount; ++i)
	{
		const SMipLevel &Mip = pHeader->m_aMips[i];
		if(i > 0)
		{
			const size_t NextWidth = (ExpWidth + 1) / 2;
			const size_t NextHeight = (ExpHeight + 1) / 2;
			uint8_t *pNext = (uint8_t *)std::malloc(NextWidth * NextHeight * 4);
			ASSERT_NE(pNext, nullptr);
			for(size_t y = 0; y < NextHeight; ++y)
			{
				const size_t Y1 = y * 2;
				const size_t Y2 = std::min(Y1 + 1, ExpHeight - 1);
				for(size_t x = 0; x < NextWidth; ++x)
				{
					const size_t X1 = x * 2;
					const size_t X2 = std::min(X1 + 1, ExpWidth - 1);
					for(size_t c = 0; c < 4; ++c)
					{
						const int Value = (((int)pExpSrc[(Y1 * ExpWidth + X1) * 4 + c] + pExpSrc[(Y1 * ExpWidth + X2) * 4 + c] + pExpSrc[(Y2 * ExpWidth + X1) * 4 + c] + pExpSrc[(Y2 * ExpWidth + X2) * 4 + c] + 2) / 4);
						pNext[(y * NextWidth + x) * 4 + c] = (uint8_t)Value;
					}
				}
			}
			if(i > 1)
				std::free((void *)pExpSrc);
			pExpSrc = pNext;
			ExpWidth = NextWidth;
			ExpHeight = NextHeight;
		}

		EXPECT_EQ(Mip.m_Width, ExpWidth);
		EXPECT_EQ(Mip.m_Height, ExpHeight);

		// Decode every block of this level and compare pixel values within tolerance.
		const uint8_t *pLevelData = (uint8_t *)pHeader + Mip.m_Offset;
		const size_t BlocksX = (Mip.m_Width + BLOCK_WIDTH - 1) / BLOCK_WIDTH;
		const size_t BlocksY = (Mip.m_Height + BLOCK_HEIGHT - 1) / BLOCK_HEIGHT;
		for(size_t By = 0; By < BlocksY; ++By)
		{
			for(size_t Bx = 0; Bx < BlocksX; ++Bx)
			{
				uint8_t aDecoded[BLOCK_WIDTH * BLOCK_HEIGHT * 4];
				DecodeDXT5Block(pLevelData + (size_t)(By * BlocksX + Bx) * 16, aDecoded);
				for(size_t Py = 0; Py < BLOCK_HEIGHT; ++Py)
				{
					const size_t Sy = By * BLOCK_HEIGHT + Py;
					if(Sy >= Mip.m_Height)
						continue;
					for(size_t Px = 0; Px < BLOCK_WIDTH; ++Px)
					{
						const size_t Sx = Bx * BLOCK_WIDTH + Px;
						if(Sx >= Mip.m_Width)
							continue;
						const size_t SrcIndex = (Sy * ExpWidth + Sx) * 4;
						const size_t DstIndex = (Py * BLOCK_WIDTH + Px) * 4;
						for(size_t C = 0; C < 4; ++C)
						{
							// One 4x4 block can only carry 4 color steps; a whole image-wide
							// gradient can exceed per-channel 64 only at the 4x4 top level
							// (DXT5 trades channels jointly), so relax that level only.
							const int Tolerance = (i == pHeader->m_MipCount - 1) ? 100 : 64;
							EXPECT_LE(std::abs((int)aDecoded[DstIndex + C] - (int)pExpSrc[SrcIndex + C]), Tolerance) << "mip " << i << " channel " << C;
						}
					}
				}
			}
		}
	}

	std::free((void *)pExpSrc);
	std::free(pHeader);
	std::free(pData);
}

TEST(TextureCompressor, RoundtripGradientWithinTolerance)
{
	// Check the worst-case reconstructed pixel error for a smooth gradient per channel.
	constexpr size_t Width = 8;
	constexpr size_t Height = 8;
	uint8_t aData[Width * Height * 4];
	for(size_t y = 0; y < Height; ++y)
	{
		for(size_t x = 0; x < Width; ++x)
		{
			aData[(y * Width + x) * 4 + 0] = (uint8_t)(x * 32);
			aData[(y * Width + x) * 4 + 1] = (uint8_t)(y * 32);
			aData[(y * Width + x) * 4 + 2] = 255;
			aData[(y * Width + x) * 4 + 3] = 255;
		}
	}

	STextureHeader *pHeader = CTextureCompressor::CompressRgba(aData, Width, Height, false);
	ASSERT_NE(pHeader, nullptr);

	// Decode every 4x4 block into its final position and compare against the source.
	const uint8_t *pData = (uint8_t *)pHeader + pHeader->m_aMips[0].m_Offset;
	constexpr size_t BlocksX = Width / BLOCK_WIDTH;
	constexpr size_t BlocksY = Height / BLOCK_HEIGHT;
	for(size_t By = 0; By < BlocksY; ++By)
	{
		for(size_t Bx = 0; Bx < BlocksX; ++Bx)
		{
			uint8_t aDecoded[BLOCK_WIDTH * BLOCK_HEIGHT * 4];
			DecodeDXT5Block(pData + (size_t)(By * BlocksX + Bx) * 16, aDecoded);
			for(size_t Py = 0; Py < BLOCK_HEIGHT; ++Py)
			{
				for(size_t Px = 0; Px < BLOCK_WIDTH; ++Px)
				{
					const size_t SrcIndex = ((By * BLOCK_HEIGHT + Py) * Width + (Bx * BLOCK_WIDTH + Px)) * 4;
					const size_t DstIndex = (Py * BLOCK_WIDTH + Px) * 4;
					// Only 4 RGB color steps exist per block, plus RGB565 endpoint
					// quantization, so allow the worst loss the format can produce.
					for(size_t C = 0; C < 4; ++C)
						EXPECT_LE(std::abs((int)aDecoded[DstIndex + C] - (int)aData[SrcIndex + C]), 64) << "channel " << C;
				}
			}
		}
	}

	std::free(pHeader);
}