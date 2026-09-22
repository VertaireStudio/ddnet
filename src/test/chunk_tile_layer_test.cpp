#include "test.h"

#include <game/map/render_layer.h>

#include <gtest/gtest.h>

namespace
{

// flat dense reference layer, and naive direct scans over it
class CDenseLayer
{
public:
	uint32_t m_Width = 0;
	uint32_t m_Height = 0;
	std::vector<uint32_t> m_aIndex;
	std::vector<uint32_t> m_aFlags;

	bool Present(uint32_t X, uint32_t Y) const
	{
		return X < m_Width && Y < m_Height && m_aIndex[Y * m_Width + X] > 0;
	}

	uint32_t BlocksInRect(uint32_t X0, uint32_t Y0, uint32_t X1, uint32_t Y1) const
	{
		uint32_t Count = 0;
		for(uint32_t Y = Y0; Y < std::min(Y1, m_Height); ++Y)
			for(uint32_t X = X0; X < std::min(X1, m_Width); ++X)
				Count += Present(X, Y);
		return Count;
	}

	uint32_t BlocksBefore(uint32_t X, uint32_t Y) const
	{
		if(Y >= m_Height)
			return BlocksInRect(0, 0, m_Width, m_Height);
		if(X >= m_Width)
			return BlocksInRect(0, 0, m_Width, Y + 1);
		return BlocksInRect(0, 0, m_Width, Y) + BlocksInRect(0, Y, X, Y + 1);
	}
};

// build a chunk layer the same way the renderer does: count per chunk, then pick
// sparse or dense, and fill
static void BuildChunked(CChunkTileLayer *pChunks, const CDenseLayer &Layer)
{
	const int Width = Layer.m_Width;
	const int Height = Layer.m_Height;
	const int ChunkSize = CChunkTileLayer::CHUNK_SIZE;
	const int ChunksX = (Width + ChunkSize - 1) / ChunkSize;
	const int ChunksY = (Height + ChunkSize - 1) / ChunkSize;

	pChunks->Init(ChunksX, ChunksY, Width, Height);

	for(int ChunkY = 0; ChunkY < ChunksY; ++ChunkY)
	{
		for(int ChunkX = 0; ChunkX < ChunksX; ++ChunkX)
		{
			int Count = 0;
			for(int Y = ChunkY * ChunkSize; Y < std::min((ChunkY + 1) * ChunkSize, Height); ++Y)
				for(int X = ChunkX * ChunkSize; X < std::min((ChunkX + 1) * ChunkSize, Width); ++X)
					Count += Layer.Present(X, Y);
			if(Count == 0)
				continue;
			CChunkTileLayer::SChunk &Chunk = pChunks->Chunk(pChunks->ChunkId(ChunkX, ChunkY));
			Chunk.m_Count = Count;
			Chunk.m_Container = (uint32_t)Count <= CChunkTileLayer::SPARSE_THRESHOLD ? CChunkTileLayer::CONTAINER_SPARSE : CChunkTileLayer::CONTAINER_DENSE;
		}
	}

	for(int ChunkY = 0; ChunkY < ChunksY; ++ChunkY)
	{
		for(int ChunkX = 0; ChunkX < ChunksX; ++ChunkX)
		{
			CChunkTileLayer::SChunk &Chunk = pChunks->Chunk(pChunks->ChunkId(ChunkX, ChunkY));
			if(Chunk.m_Container == CChunkTileLayer::CONTAINER_SPARSE)
			{
				Chunk.m_aSparse.reserve(Chunk.m_Count);
				for(int Y = ChunkY * ChunkSize; Y < std::min((ChunkY + 1) * ChunkSize, Height); ++Y)
				{
					for(int X = ChunkX * ChunkSize; X < std::min((ChunkX + 1) * ChunkSize, Width); ++X)
					{
						if(Layer.Present(X, Y))
							Chunk.m_aSparse.emplace_back(CChunkTileLayer::SChunkTile::Make(X & CChunkTileLayer::X_MASK, Y & CChunkTileLayer::Y_MASK, Layer.m_aIndex[Y * Width + X], Layer.m_aFlags[Y * Width + X]));
					}
				}
			}
			else if(Chunk.m_Container == CChunkTileLayer::CONTAINER_DENSE)
			{
				for(int Y = ChunkY * ChunkSize; Y < std::min((ChunkY + 1) * ChunkSize, Height); ++Y)
				{
					for(int X = ChunkX * ChunkSize; X < std::min((ChunkX + 1) * ChunkSize, Width); ++X)
					{
						if(Layer.Present(X, Y))
							Chunk.m_aDense[(Y & CChunkTileLayer::Y_MASK) * CChunkTileLayer::CHUNK_SIZE + (X & CChunkTileLayer::X_MASK)] = (uint16_t)((Layer.m_aIndex[Y * Width + X] << 8) | Layer.m_aFlags[Y * Width + X]);
					}
				}
			}
		}
	}

	pChunks->BuildRowPrefix();
}

static void ExpectMatches(const CChunkTileLayer &Chunks, const CDenseLayer &Layer)
{
	EXPECT_EQ(Chunks.BlocksInRect(0, 0, Layer.m_Width, Layer.m_Height), Layer.BlocksInRect(0, 0, Layer.m_Width, Layer.m_Height));

	for(uint32_t Y = 0; Y < Layer.m_Height; ++Y)
	{
		for(uint32_t X = 0; X < Layer.m_Width; ++X)
		{
			EXPECT_EQ(Chunks.Present(X, Y), Layer.Present(X, Y)) << X << " " << Y;
			unsigned char Index = 0;
			unsigned char Flags = 0xFF;
			Chunks.Get(X, Y, &Index, &Flags);
			EXPECT_EQ((uint32_t)Index, Layer.m_aIndex[Y * Layer.m_Width + X]) << X << " " << Y;
			EXPECT_EQ((uint32_t)Flags, Layer.m_aFlags[Y * Layer.m_Width + X]) << X << " " << Y;
			EXPECT_EQ(Chunks.BlocksBefore(X, Y), Layer.BlocksBefore(X, Y)) << X << " " << Y;
		}
	}

	// a few rects that cross chunk borders and partially cover chunks
	struct SRect
	{
		uint32_t X0, Y0, X1, Y1;
	};
	const SRect aRects[] = {
		{0, 0, Layer.m_Width, Layer.m_Height},
		{5, 7, 40, 70},
		{0, 0, 12, 12},
		{30, 30, 64, 64},
		{33, 65, 100, 100},
		{1, 0, Layer.m_Width, 1},
		{0, 1, 1, Layer.m_Height},
		{7, 0, 8, Layer.m_Height},   // column strip
		{0, 9, Layer.m_Width, 10},   // single wide row
		{0, 9, 1, 10},               // single cell
		{17, 31, 21, 64},            // partial chunk strip
		{Layer.m_Width / 2, Layer.m_Height / 3, Layer.m_Width, Layer.m_Height},
	};
	for(const auto &Rect : aRects)
	{
		if(Rect.X0 >= Rect.X1 || Rect.Y0 >= Rect.Y1 || Rect.X0 >= Layer.m_Width || Rect.Y0 >= Layer.m_Height)
			continue;
		EXPECT_EQ(Chunks.BlocksInRect(Rect.X0, Rect.Y0, Rect.X1, Rect.Y1), Layer.BlocksInRect(Rect.X0, Rect.Y0, Rect.X1, Rect.Y1)) << Rect.X0 << " " << Rect.Y0 << " " << Rect.X1 << " " << Rect.Y1;
	}

	// random rects
	uint32_t State = 12345;
	auto Rand = [&]() {
		State = State * 1103515245u + 12345u;
		return (State >> 16) & 0x7FFF;
	};
	for(int i = 0; i < 2000; ++i)
	{
		const uint32_t X0 = Rand() % (Layer.m_Width + 8);
		const uint32_t Y0 = Rand() % (Layer.m_Height + 8);
		const uint32_t X1 = X0 + Rand() % (Layer.m_Width + 8);
		const uint32_t Y1 = Y0 + Rand() % (Layer.m_Height + 8);
		EXPECT_EQ(Chunks.BlocksInRect(X0, Y0, X1, Y1), Layer.BlocksInRect(X0, Y0, X1, Y1)) << X0 << " " << Y0 << " " << X1 << " " << Y1;
	}

	// queries that reach past the edges must behave like a cell just outside the layer
	EXPECT_EQ(Chunks.BlocksBefore(Layer.m_Width, Layer.m_Height / 2), Layer.BlocksBefore(Layer.m_Width, Layer.m_Height / 2));
	EXPECT_EQ(Chunks.BlocksBefore(0, Layer.m_Height + 10), Layer.BlocksBefore(0, Layer.m_Height + 10));
	EXPECT_EQ(Chunks.BlocksInRect(0, 0, Layer.m_Width + 50, Layer.m_Height + 50), Layer.BlocksInRect(0, 0, Layer.m_Width + 50, Layer.m_Height + 50));
}

// deterministic pseudo random fill
static void FillRandom(CDenseLayer *pLayer, uint32_t DensityPercent, uint32_t Seed)
{
	uint32_t State = Seed;
	auto Rand = [&]() {
		State = State * 1103515245u + 12345u;
		return (State >> 16) & 0x7FFF;
	};
	for(uint32_t i = 0; i < pLayer->m_Width * pLayer->m_Height; ++i)
	{
		if(Rand() % 100 < DensityPercent)
		{
			pLayer->m_aIndex[i] = 1 + Rand() % 255;
			pLayer->m_aFlags[i] = Rand() % 16;
		}
		else
		{
			pLayer->m_aIndex[i] = 0;
			pLayer->m_aFlags[i] = 0;
		}
	}
}

TEST(ChunkTileLayer, EmptyLayer)
{
	CDenseLayer Layer;
	Layer.m_Width = 10;
	Layer.m_Height = 10;
	Layer.m_aIndex.assign(100, 0);
	Layer.m_aFlags.assign(100, 0);

	CChunkTileLayer Chunks;
	BuildChunked(&Chunks, Layer);
	ExpectMatches(Chunks, Layer);
}

TEST(ChunkTileLayer, SparseAcrossChunks)
{
	CDenseLayer Layer;
	Layer.m_Width = 70;
	Layer.m_Height = 50;
	Layer.m_aIndex.assign(Layer.m_Width * Layer.m_Height, 0);
	Layer.m_aFlags.assign(Layer.m_Width * Layer.m_Height, 0);
	FillRandom(&Layer, 8, 1);

	CChunkTileLayer Chunks;
	BuildChunked(&Chunks, Layer);
	ExpectMatches(Chunks, Layer);
}

TEST(ChunkTileLayer, PartialChunks)
{
	// odd sizes so the bottom and right chunks are partial
	CDenseLayer Layer;
	Layer.m_Width = 33;
	Layer.m_Height = 65;
	Layer.m_aIndex.assign(Layer.m_Width * Layer.m_Height, 0);
	Layer.m_aFlags.assign(Layer.m_Width * Layer.m_Height, 0);
	Layer.m_aIndex[0] = 1;
	Layer.m_aFlags[0] = 9;
	Layer.m_aIndex[Layer.m_Height * Layer.m_Width - 1] = 7;
	Layer.m_aFlags[Layer.m_Height * Layer.m_Width - 1] = 3;
	Layer.m_aIndex[33] = 12; // first cell of row 1
	Layer.m_aFlags[33] = 5;

	CChunkTileLayer Chunks;
	BuildChunked(&Chunks, Layer);
	ExpectMatches(Chunks, Layer);
}

TEST(ChunkTileLayer, DenseChunks)
{
	// completely fill two 32x32 chunks so they flip to the dense grid
	CDenseLayer Layer;
	Layer.m_Width = 100;
	Layer.m_Height = 70;
	Layer.m_aIndex.assign(Layer.m_Width * Layer.m_Height, 0);
	Layer.m_aFlags.assign(Layer.m_Width * Layer.m_Height, 0);
	for(uint32_t Y = 0; Y < 32; ++Y)
		for(uint32_t X = 0; X < 32; ++X)
		{
			Layer.m_aIndex[Y * Layer.m_Width + X] = 1 + (X * 7 + Y) % 255;
			Layer.m_aFlags[Y * Layer.m_Width + X] = (X + Y) % 16;
		}
	for(uint32_t Y = 32; Y < 64; ++Y)
		for(uint32_t X = 64; X < 96; ++X)
			Layer.m_aIndex[Y * Layer.m_Width + X] = 1 + (X * 3 + Y * 5) % 255;
	Layer.m_aIndex[32 * Layer.m_Width + 5] = 20;
	Layer.m_aFlags[32 * Layer.m_Width + 5] = 8;

	CChunkTileLayer Chunks;
	BuildChunked(&Chunks, Layer);
	ExpectMatches(Chunks, Layer);
}

TEST(ChunkTileLayer, SparseDenseBoundary)
{
	// a chunk right at the sparse/dense threshold must still answer identically
	for(uint32_t Count : {CChunkTileLayer::SPARSE_THRESHOLD, CChunkTileLayer::SPARSE_THRESHOLD + 1})
	{
		CDenseLayer Layer;
		Layer.m_Width = 32;
		Layer.m_Height = 64;
		Layer.m_aIndex.assign(Layer.m_Width * Layer.m_Height, 0);
		Layer.m_aFlags.assign(Layer.m_Width * Layer.m_Height, 0);
		uint32_t Filled = 0;
		for(uint32_t Y = 0; Y < 32 && Filled < Count; ++Y)
			for(uint32_t X = 0; X < 32 && Filled < Count; ++X)
			{
				Layer.m_aIndex[Y * Layer.m_Width + X] = (Filled % 255) + 1;
				Layer.m_aFlags[Y * Layer.m_Width + X] = (X + Y) % 16;
				++Filled;
			}

		CChunkTileLayer Chunks;
		BuildChunked(&Chunks, Layer);
		EXPECT_EQ(Chunks.Chunk(Chunks.ChunkId(0, 0)).m_Container, Count <= CChunkTileLayer::SPARSE_THRESHOLD ? CChunkTileLayer::CONTAINER_SPARSE : CChunkTileLayer::CONTAINER_DENSE);
		ExpectMatches(Chunks, Layer);
	}
}

TEST(ChunkTileLayer, RowSplitHelpers)
{
	// validate RowPrefix/ColsBefore/RowExtras against the dense oracle, with
	// partial edge chunks, a fully-filled dense chunk, and W%32==0 boundaries
	struct SCase
	{
		uint32_t Width, Height;
	};
	for(const SCase &Case : {SCase{200u, 137u}, SCase{64u, 40u}})
	{
		CDenseLayer Layer;
		Layer.m_Width = Case.Width;
		Layer.m_Height = Case.Height;
		Layer.m_aIndex.assign(Layer.m_Width * Layer.m_Height, 0);
		Layer.m_aFlags.assign(Layer.m_Width * Layer.m_Height, 0);
		for(uint32_t Y = 0; Y < Layer.m_Height; ++Y)
			for(uint32_t X = 0; X < Layer.m_Width; ++X)
				if((X * 7 + Y * 13) % 5 == 0 || (X / 32 == 2 && Y / 32 == 2))
				{
					Layer.m_aIndex[Y * Layer.m_Width + X] = ((X * 3 + Y) % 255) + 1;
					Layer.m_aFlags[Y * Layer.m_Width + X] = (X + Y) % 16;
				}

		CChunkTileLayer Chunks;
		BuildChunked(&Chunks, Layer);

		for(uint32_t Y = 0; Y <= Layer.m_Height; ++Y)
			EXPECT_EQ(Chunks.RowPrefix(Y), Layer.BlocksInRect(0, 0, Layer.m_Width, Y)) << Case.Width << " " << Y;

		for(uint32_t Y = 0; Y < Layer.m_Height; ++Y)
		{
			for(uint32_t X : {0u, 1u, 31u, 32u, 33u, 63u, 64u, 65u, Layer.m_Width})
			{
				EXPECT_EQ(Chunks.ColsBefore(X, Y), Layer.BlocksInRect(0, Y, X, Y + 1)) << Case.Width << " " << X << " " << Y;
				EXPECT_EQ(Chunks.RowPrefix(Y) + Chunks.ColsBefore(X, Y), Chunks.BlocksBefore(X, Y)) << Case.Width << " " << X << " " << Y;
			}
			for(uint32_t X0 : {0u, 31u, 32u, 65u, Layer.m_Width - 1})
			{
				for(uint32_t X1 : {X0 + 1, 64u, Layer.m_Width})
				{
					if(X1 <= X0)
						continue;
					EXPECT_EQ(Chunks.RowExtras(X0, X1, Y), Layer.BlocksInRect(X0, Y, X1, Y + 1)) << Case.Width << " " << X0 << " " << X1 << " " << Y;
					EXPECT_EQ(Chunks.ColsBefore(X1, Y) - Chunks.ColsBefore(X0, Y), Chunks.RowExtras(X0, X1, Y)) << Case.Width << " " << X0 << " " << X1 << " " << Y;
				}
			}
		}

		EXPECT_EQ(Chunks.RowExtras(0, 0, 0), 0u);
		EXPECT_EQ(Chunks.RowExtras(Layer.m_Width / 2, Layer.m_Width / 2 + 10, Layer.m_Height), 0u);
		EXPECT_EQ(Chunks.ColsBefore(Layer.m_Width, Layer.m_Height), 0u);
	}
}

} // namespace