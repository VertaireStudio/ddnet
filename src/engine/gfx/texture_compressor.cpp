#include "texture_compressor.h"

#include <algorithm>
#include <cstdlib>
using CTextureCompressor::BLOCK_HEIGHT;
using CTextureCompressor::BLOCK_SIZE;
using CTextureCompressor::BLOCK_WIDTH;
using CTextureCompressor::SMipLevel;
using CTextureCompressor::STextureHeader;

namespace
{
	static void GenerateMipLevel(uint8_t *pDest, size_t DestWidth, size_t DestHeight, const uint8_t *pSrc, size_t SrcWidth, size_t SrcHeight)
	{
		for(size_t y = 0; y < DestHeight; ++y)
		{
			const size_t SrcY = y * 2;
			const size_t SrcY2 = std::min(SrcY + 1, SrcHeight - 1);
			for(size_t x = 0; x < DestWidth; ++x)
			{
				const size_t SrcX = x * 2;
				const size_t SrcX2 = std::min(SrcX + 1, SrcWidth - 1);
				const size_t Index00 = (SrcY * SrcWidth + SrcX) * 4;
				const size_t Index01 = (SrcY * SrcWidth + SrcX2) * 4;
				const size_t Index10 = (SrcY2 * SrcWidth + SrcX) * 4;
				const size_t Index11 = (SrcY2 * SrcWidth + SrcX2) * 4;
				for(size_t i = 0; i < 4; ++i)
				{
					const unsigned Value = (pSrc[Index00 + i] + pSrc[Index01 + i] + pSrc[Index10 + i] + pSrc[Index11 + i] + 2) / 4;
					pDest[(y * DestWidth + x) * 4 + i] = (uint8_t)Value;
				}
			}
		}
	}

	// Copies a plane into a larger one, replicating the edge pixels for padding.
	static void BuildPaddedPlane(uint8_t *pDest, size_t DestWidth, size_t DestHeight, const uint8_t *pSrc, size_t SrcWidth, size_t SrcHeight)
	{
		for(size_t y = 0; y < DestHeight; ++y)
		{
			const size_t SrcY = std::min(y, SrcHeight - 1);
			for(size_t x = 0; x < DestWidth; ++x)
			{
				const size_t SrcX = std::min(x, SrcWidth - 1);
				const size_t SrcOffset = (SrcY * SrcWidth + SrcX) * 4;
				const size_t DstOffset = (y * DestWidth + x) * 4;
				pDest[DstOffset + 0] = pSrc[SrcOffset + 0];
				pDest[DstOffset + 1] = pSrc[SrcOffset + 1];
				pDest[DstOffset + 2] = pSrc[SrcOffset + 2];
				pDest[DstOffset + 3] = pSrc[SrcOffset + 3];
			}
		}
	}

	static size_t AlignUp(size_t Value, size_t Alignment)
	{
		return ((Value + Alignment - 1) / Alignment) * Alignment;
	}

	static void Expand565(uint16_t Value, int &R, int &G, int &B)
	{
		const int R5 = (Value >> 11) & 31;
		const int G6 = (Value >> 5) & 63;
		const int B5 = Value & 31;
		R = (R5 << 3) | (R5 >> 2);
		G = (G6 << 2) | (G6 >> 4);
		B = (B5 << 3) | (B5 >> 2);
	}

	static uint16_t Pack565(int R, int G, int B)
	{
		const int R5 = std::min(std::max(R, 0), 255) >> 3;
		const int G6 = std::min(std::max(G, 0), 255) >> 2;
		const int B5 = std::min(std::max(B, 0), 255) >> 3;
		return (uint16_t)((R5 << 11) | (G6 << 5) | B5);
	}

	// Encode the 8 alpha bytes of a DXT5 block from a 4x4 region of the plane.
	// Reads out of bounds up to the padded plane size; the caller guarantees enough padding.
	static void EncodeBlockAlpha(const uint8_t *pSrc, size_t Pitch, uint8_t *pDst)
	{
		uint8_t aAlpha[BLOCK_WIDTH * BLOCK_HEIGHT];
		uint8_t Min = 255;
		uint8_t Max = 0;
		for(size_t y = 0; y < BLOCK_HEIGHT; ++y)
		{
			for(size_t x = 0; x < BLOCK_WIDTH; ++x)
			{
				const uint8_t Value = pSrc[y * Pitch + x * 4 + 3];
				aAlpha[y * BLOCK_WIDTH + x] = Value;
				Min = std::min(Min, Value);
				Max = std::max(Max, Value);
			}
		}

		// Use the 8-entry min<=max ramp (indices 2..5 are interpolants, 6 -> 0, 7 -> 255).
		pDst[0] = Min;
		pDst[1] = Max;
		const int aRamp[8] = {
			(int)Min,
			(int)Max,
			(4 * (int)Min + 1 * (int)Max + 2) / 5,
			(3 * (int)Min + 2 * (int)Max + 2) / 5,
			(2 * (int)Min + 3 * (int)Max + 2) / 5,
			(1 * (int)Min + 4 * (int)Max + 2) / 5,
			0,
			255,
		};

		uint64_t Indices = 0;
		for(size_t i = 0; i < BLOCK_WIDTH * BLOCK_HEIGHT; ++i)
		{
			int BestIndex = 0;
			int BestDiff = 1 << 30;
			for(int Index = 0; Index < 8; ++Index)
			{
				const int Diff = std::abs((int)aAlpha[i] - aRamp[Index]);
				if(Diff < BestDiff)
				{
					BestDiff = Diff;
					BestIndex = Index;
				}
			}
			Indices |= (uint64_t)BestIndex << (3 * i);
		}
		for(size_t i = 0; i < 6; ++i)
			pDst[2 + i] = (uint8_t)((Indices >> (8 * i)) & 0xff);
	}

	// Encode the 8 color bytes of a DXT5 block from a 4x4 region of the plane.
	static void EncodeBlockColor(const uint8_t *pSrc, size_t Pitch, uint8_t *pDst)
	{
		uint8_t aR[BLOCK_WIDTH * BLOCK_HEIGHT];
		uint8_t aG[BLOCK_WIDTH * BLOCK_HEIGHT];
		uint8_t aB[BLOCK_WIDTH * BLOCK_HEIGHT];
		for(size_t y = 0; y < BLOCK_HEIGHT; ++y)
		{
			for(size_t x = 0; x < BLOCK_WIDTH; ++x)
			{
				const size_t Index = y * BLOCK_WIDTH + x;
				const size_t Offset = y * Pitch + x * 4;
				aR[Index] = pSrc[Offset + 0];
				aG[Index] = pSrc[Offset + 1];
				aB[Index] = pSrc[Offset + 2];
			}
		}

		// Bounding-box endpoints
		int MinR = 255, MinG = 255, MinB = 255;
		int MaxR = 0, MaxG = 0, MaxB = 0;
		for(size_t i = 0; i < BLOCK_WIDTH * BLOCK_HEIGHT; ++i)
		{
			MinR = std::min(MinR, (int)aR[i]);
			MinG = std::min(MinG, (int)aG[i]);
			MinB = std::min(MinB, (int)aB[i]);
			MaxR = std::max(MaxR, (int)aR[i]);
			MaxG = std::max(MaxG, (int)aG[i]);
			MaxB = std::max(MaxB, (int)aB[i]);
		}

		uint16_t e0 = Pack565(MinR, MinG, MinB);
		uint16_t e1 = Pack565(MaxR, MaxG, MaxB);
		// Force 4-color mode: the DXT1/3-color layout maps index 3 to transparent black.
		uint16_t c0, c1;
		if(e0 > e1)
		{
			c0 = e0;
			c1 = e1;
		}
		else
		{
			c0 = e1;
			c1 = e0;
		}

		int R0, G0, B0, R1, G1, B1;
		Expand565(c0, R0, G0, B0);
		Expand565(c1, R1, G1, B1);
		int aPaletteR[4] = {R0, R1, (2 * R0 + R1) / 3, (R0 + 2 * R1) / 3};
		int aPaletteG[4] = {G0, G1, (2 * G0 + G1) / 3, (G0 + 2 * G1) / 3};
		int aPaletteB[4] = {B0, B1, (2 * B0 + B1) / 3, (B0 + 2 * B1) / 3};

		pDst[0] = c0 & 0xff;
		pDst[1] = c0 >> 8;
		pDst[2] = c1 & 0xff;
		pDst[3] = c1 >> 8;

		uint32_t Indices = 0;
		for(size_t i = 0; i < BLOCK_WIDTH * BLOCK_HEIGHT; ++i)
		{
			int BestIndex = 0;
			long BestDiff = 1L << 30;
			for(int Index = 0; Index < 4; ++Index)
			{
				const int DR = (int)aR[i] - aPaletteR[Index];
				const int DG = (int)aG[i] - aPaletteG[Index];
				const int DB = (int)aB[i] - aPaletteB[Index];
				const long Diff = (long)DR * DR + (long)DG * DG + (long)DB * DB;
				if(Diff < BestDiff)
				{
					BestDiff = Diff;
					BestIndex = Index;
				}
			}
			Indices |= (uint32_t)BestIndex << (2 * i);
		}
		for(size_t i = 0; i < 4; ++i)
			pDst[4 + i] = (uint8_t)((Indices >> (8 * i)) & 0xff);
	}

	static void EncodeBlockRGBA(const uint8_t *pSrc, size_t Pitch, uint8_t *pDst)
	{
		EncodeBlockAlpha(pSrc, Pitch, pDst);
		EncodeBlockColor(pSrc, Pitch, pDst + 8);
	}

	static size_t BlockSpan(size_t Width, size_t Height)
	{
		return ((Width + BLOCK_WIDTH - 1) / BLOCK_WIDTH) * ((Height + BLOCK_HEIGHT - 1) / BLOCK_HEIGHT);
	}
}

STextureHeader *CTextureCompressor::CompressRgba(const uint8_t *pBaseData, size_t Width, size_t Height, bool GenerateMipmaps)
{
	if(pBaseData == nullptr || Width == 0 || Height == 0 || Width % BLOCK_WIDTH != 0 || Height % BLOCK_HEIGHT != 0)
		return nullptr;

	// Collect the mip level dimensions: base first, halving each time, stopping at 4x4.
	size_t aLevelWidths[MAX_MIP_LEVELS];
	size_t aLevelHeights[MAX_MIP_LEVELS];
	size_t MipCount = 0;
	size_t CursorWidth = Width;
	size_t CursorHeight = Height;
	while(true)
	{
		aLevelWidths[MipCount] = CursorWidth;
		aLevelHeights[MipCount] = CursorHeight;
		++MipCount;
		if(!GenerateMipmaps || (CursorWidth <= BLOCK_WIDTH && CursorHeight <= BLOCK_HEIGHT))
			break;
		CursorWidth = (CursorWidth + 1) / 2;
		CursorHeight = (CursorHeight + 1) / 2;
	}

	size_t TotalDataSize = 0;
	for(size_t i = 0; i < MipCount; ++i)
		TotalDataSize += BlockSpan(aLevelWidths[i], aLevelHeights[i]) * BLOCK_SIZE;

	STextureHeader *pHeader = (STextureHeader *)std::malloc(sizeof(STextureHeader) + TotalDataSize);
	if(pHeader == nullptr)
		return nullptr;
	pHeader->m_MipCount = MipCount;

	// Blocks are the size of the file system pages, but the encoders operate on
	// 4x4 blocks, so we work on planes padded to a multiple of 4.
	uint8_t *pWriteCursor = (uint8_t *)pHeader + sizeof(STextureHeader);
	const uint8_t *pTruePlane = pBaseData; // true data of the current level
	bool bPlaneIsHeapAllocated = false;

	for(size_t i = 0; i < MipCount; ++i)
	{
		SMipLevel &Mip = pHeader->m_aMips[i];
		Mip.m_Width = aLevelWidths[i];
		Mip.m_Height = aLevelHeights[i];
		Mip.m_Offset = (size_t)(pWriteCursor - (uint8_t *)pHeader);
		Mip.m_DataSize = BlockSpan(Mip.m_Width, Mip.m_Height) * BLOCK_SIZE;

		// Encode into a padded copy so block reads never walk off the level.
		const size_t PaddedWidth = AlignUp(Mip.m_Width, BLOCK_WIDTH);
		const size_t PaddedHeight = AlignUp(Mip.m_Height, BLOCK_HEIGHT);
		uint8_t *pPaddedPlane = (uint8_t *)std::malloc(PaddedWidth * PaddedHeight * 4);
		if(pPaddedPlane == nullptr)
		{
			if(bPlaneIsHeapAllocated)
				std::free((void *)pTruePlane);
			std::free(pHeader);
			return nullptr;
		}
		// The base level is passed in uncompressed, the generated levels are upsized
		// into the padded plane with replicated edge pixels at block boundaries.
		BuildPaddedPlane(pPaddedPlane, PaddedWidth, PaddedHeight, pTruePlane, aLevelWidths[i], aLevelHeights[i]);

		const size_t BlocksX = (Mip.m_Width + BLOCK_WIDTH - 1) / BLOCK_WIDTH;
		const size_t BlocksY = (Mip.m_Height + BLOCK_HEIGHT - 1) / BLOCK_HEIGHT;
		for(size_t By = 0; By < BlocksY; ++By)
		{
			for(size_t Bx = 0; Bx < BlocksX; ++Bx)
			{
				EncodeBlockRGBA(pPaddedPlane + (By * BLOCK_HEIGHT) * PaddedWidth * 4 + (Bx * BLOCK_WIDTH) * 4, PaddedWidth * 4, pWriteCursor);
				pWriteCursor += BLOCK_SIZE;
			}
		}
		std::free(pPaddedPlane);

		if((i + 1) < MipCount)
		{
			const size_t NextWidth = aLevelWidths[i + 1];
			const size_t NextHeight = aLevelHeights[i + 1];
			uint8_t *pNextPlane = (uint8_t *)std::malloc(NextWidth * NextHeight * 4);
			if(pNextPlane == nullptr)
			{
				if(bPlaneIsHeapAllocated)
					std::free((void *)pTruePlane);
				std::free(pHeader);
				return nullptr;
			}
			GenerateMipLevel(pNextPlane, NextWidth, NextHeight, pTruePlane, aLevelWidths[i], aLevelHeights[i]);
			if(bPlaneIsHeapAllocated)
				std::free((void *)pTruePlane);
			pTruePlane = pNextPlane;
			bPlaneIsHeapAllocated = true;
		}
	}
	if(bPlaneIsHeapAllocated)
		std::free((void *)pTruePlane);

	return pHeader;
}