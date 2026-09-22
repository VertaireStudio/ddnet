#ifndef GAME_MAP_RENDER_LAYER_H
#define GAME_MAP_RENDER_LAYER_H

#include <cstdint>

using offset_ptr_size = char *;
using offset_ptr = uintptr_t;
using offset_ptr32 = unsigned int;

#include <base/color.h>

#include <engine/graphics.h>

#include <game/map/envelope_manager.h>
#include <game/map/render_component.h>
#include <game/map/render_map.h>
#include <game/mapitems.h>
#include <game/mapitems_ex.h>

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <vector>

class CMapLayers;
class CMapItemLayerTilemap;
class CMapItemLayerQuads;
class IMap;
class CMapImages;

typedef std::function<void(int GroupId, int LayerId)> FCallbackLayerInit;

constexpr int BorderRenderDistance = 201;

class CClipRegion
{
public:
	CClipRegion() = default;
	CClipRegion(float X, float Y, float Width, float Height) :
		m_X(X), m_Y(Y), m_Width(Width), m_Height(Height) {}

	float m_X;
	float m_Y;
	float m_Width;
	float m_Height;
};

class CRenderLayerParams
{
public:
	int m_RenderType;
	int m_EntityOverlayVal;
	vec2 m_Center;
	float m_Zoom;
	bool m_RenderText;
	bool m_RenderInvalidTiles;
	bool m_TileAndQuadBuffering;
	bool m_RenderTileBorder;
	bool m_DebugRenderGroupClips;
	bool m_DebugRenderQuadClips;
	bool m_DebugRenderClusterClips;
	bool m_DebugRenderTileClips;
};

// Dense tile layers are stored as one 4-byte CTile per cell, even when almost
// every cell is empty and a huge map is then a huge block of almost nothing.
// This container instead stores tiles in 32x32 chunks, empty chunks cost only
// their header and fully covered chunks flip to a plain 2-byte grid, so design
// layers keep only their drawn tiles in memory.
class CChunkTileLayer
{
public:
	static constexpr uint32_t CHUNK_POW = 5;
	static constexpr uint32_t CHUNK_SIZE = 1u << CHUNK_POW;

	// layout of the single 32-bit word one sparse tile takes up
	static constexpr uint32_t X_SHIFT = 0;
	static constexpr uint32_t X_MASK = CHUNK_SIZE - 1;
	static constexpr uint32_t Y_SHIFT = X_SHIFT + CHUNK_POW;
	static constexpr uint32_t Y_MASK = CHUNK_SIZE - 1;
	static constexpr uint32_t INDEX_SHIFT = Y_SHIFT + CHUNK_POW;
	static constexpr uint32_t INDEX_MASK = 0xFF;
	static constexpr uint32_t FLAGS_SHIFT = INDEX_SHIFT + 8;
	static constexpr uint32_t FLAGS_MASK = 0xFF;

	enum : uint32_t
	{
		CONTAINER_NONE = 0,
		CONTAINER_SPARSE,
		CONTAINER_DENSE,
	};

	class SChunkTile
	{
	public:
		SChunkTile() = default;
		SChunkTile(uint32_t Packed) :
			m_Packed(Packed) {}

		uint32_t X() const { return (m_Packed >> X_SHIFT) & X_MASK; }
		uint32_t Y() const { return (m_Packed >> Y_SHIFT) & Y_MASK; }
		uint32_t Index() const { return (m_Packed >> INDEX_SHIFT) & INDEX_MASK; }
		uint32_t Flags() const { return (m_Packed >> FLAGS_SHIFT) & FLAGS_MASK; }

		static SChunkTile Make(uint32_t X, uint32_t Y, uint32_t Index, uint32_t Flags)
		{
			return SChunkTile((X << X_SHIFT) | (Y << Y_SHIFT) | (Index << INDEX_SHIFT) | (Flags << FLAGS_SHIFT));
		}

	private:
		uint32_t m_Packed;
	};
	static_assert(sizeof(SChunkTile) == sizeof(uint32_t));

	class SChunk
	{
	public:
		SChunk()
		{
			m_aDense.fill(0);
		}

		uint32_t m_Container = CONTAINER_NONE;
		uint32_t m_Count = 0;
		std::vector<SChunkTile> m_aSparse;
		std::array<uint16_t, CHUNK_SIZE * CHUNK_SIZE> m_aDense;
	};

	// above this many drawn tiles a 2-byte grid fits the chunk better than 4-byte sparse tiles
	static constexpr uint32_t SPARSE_THRESHOLD = (CHUNK_SIZE * CHUNK_SIZE * 2) / 3;

	void Init(uint32_t ChunksX, uint32_t ChunksY, uint32_t Width, uint32_t Height)
	{
		m_ChunksX = ChunksX;
		m_ChunksY = ChunksY;
		m_Width = Width;
		m_Height = Height;
		m_vChunks.clear();
		m_vChunks.resize((size_t)ChunksX * (size_t)ChunksY);
	}

	bool IsEmpty() const { return m_vChunks.empty(); }

	size_t ChunkId(uint32_t ChunkX, uint32_t ChunkY) const { return (size_t)ChunkY * m_ChunksX + ChunkX; }
	SChunk &Chunk(size_t ChunkId) { return m_vChunks[ChunkId]; }
	const SChunk &Chunk(size_t ChunkId) const { return m_vChunks[ChunkId]; }

	// is there any drawn tile at (X, Y)?
	bool Present(uint32_t X, uint32_t Y) const
	{
		if(X >= m_Width || Y >= m_Height)
			return false;
		const SChunk &Chunk = m_vChunks[ChunkId(X >> CHUNK_POW, Y >> CHUNK_POW)];
		uint32_t Kx = X & X_MASK;
		uint32_t Ky = Y & Y_MASK;
		return CountInChunk(Chunk, Kx, Ky, Kx + 1, Ky + 1) != 0;
	}

	// index and flags of the tile at (X, Y), zeroed when the cell is empty
	void Get(uint32_t X, uint32_t Y, unsigned char *pIndex, unsigned char *pFlags) const
	{
		if(X >= m_Width || Y >= m_Height)
		{
			*pIndex = 0;
			*pFlags = 0;
			return;
		}
		const SChunk &Chunk = m_vChunks[ChunkId(X >> CHUNK_POW, Y >> CHUNK_POW)];
		if(Chunk.m_Container == CONTAINER_SPARSE)
		{
			const uint32_t Key = TileKey(X & X_MASK, Y & Y_MASK);
			auto It = std::lower_bound(Chunk.m_aSparse.begin(), Chunk.m_aSparse.end(), Key, [](const SChunkTile &Tile, uint32_t TileKey) {
				return Tile.Y() * CHUNK_SIZE + Tile.X() < TileKey;
			});
			if(It != Chunk.m_aSparse.end() && It->Y() * CHUNK_SIZE + It->X() == Key)
			{
				*pIndex = It->Index();
				*pFlags = It->Flags();
			}
			else
			{
				*pIndex = 0;
				*pFlags = 0;
			}
		}
		else if(Chunk.m_Container == CONTAINER_DENSE)
		{
			const uint16_t Value = Chunk.m_aDense[(Y & Y_MASK) * CHUNK_SIZE + (X & X_MASK)];
			*pIndex = Value >> 8;
			*pFlags = Value & 0xFF;
		}
		else
		{
			*pIndex = 0;
			*pFlags = 0;
		}
	}

	static uint32_t TileKey(uint32_t X, uint32_t Y)
	{
		return Y * CHUNK_SIZE + X;
	}

	// number of drawn tiles in the half-open cell rect [X0, X1) x [Y0, Y1)
	uint32_t BlocksInRect(uint32_t X0, uint32_t Y0, uint32_t X1, uint32_t Y1) const
	{
		if(X0 >= X1 || Y0 >= Y1)
			return 0;
		X1 = std::min(X1, m_Width);
		Y1 = std::min(Y1, m_Height);
		if(X0 >= X1 || Y0 >= Y1)
			return 0;

		uint32_t Count = 0;
		const uint32_t LastCx = (X1 - 1) >> CHUNK_POW;
		const uint32_t LastCy = (Y1 - 1) >> CHUNK_POW;
		for(uint32_t Cy = Y0 >> CHUNK_POW; Cy <= LastCy; ++Cy)
		{
			const uint32_t Row0 = Cy << CHUNK_POW;
			const uint32_t Row1 = std::min(Row0 + CHUNK_SIZE, m_Height);
			const uint32_t LocalY0 = std::max(Y0, Row0) - Row0;
			const uint32_t LocalY1 = std::min(Y1, Row1) - Row0;
			for(uint32_t Cx = X0 >> CHUNK_POW; Cx <= LastCx; ++Cx)
			{
				const uint32_t Col0 = Cx << CHUNK_POW;
				const uint32_t Col1 = std::min(Col0 + CHUNK_SIZE, m_Width);
				const uint32_t LocalX0 = std::max(X0, Col0) - Col0;
				const uint32_t LocalX1 = std::min(X1, Col1) - Col0;
				const SChunk &Chunk = m_vChunks[ChunkId(Cx, Cy)];
				if(LocalX0 == 0 && LocalY0 == 0 && LocalX1 - LocalX0 == CHUNK_SIZE && LocalY1 - LocalY0 == CHUNK_SIZE)
					Count += Chunk.m_Count;
				else
					Count += CountInChunk(Chunk, LocalX0, LocalY0, LocalX1, LocalY1);
			}
		}
		return Count;
	}

	// number of drawn tiles strictly before cell (X, Y) in row-major order.
	// Needs BuildRowPrefix() to have run after the chunks are filled
	uint32_t BlocksBefore(uint32_t X, uint32_t Y) const
	{
		if(X > m_Width)
			X = m_Width;
		if(Y > m_Height)
			Y = m_Height;
		if(Y == m_Height)
			return m_vRowPrefix[m_Height];

		const uint32_t Kx = X & X_MASK;
		const uint32_t Ky = Y & Y_MASK;
		uint32_t Count = m_vRowPrefix[Y];
		for(uint32_t C = 0; C < X >> CHUNK_POW; ++C)
			Count += CountInChunk(m_vChunks[ChunkId(C, Y >> CHUNK_POW)], 0, Ky, CHUNK_SIZE, Ky + 1);
		if(Kx > 0)
			Count += CountInChunk(m_vChunks[ChunkId(X >> CHUNK_POW, Y >> CHUNK_POW)], 0, Ky, Kx, Ky + 1);
		return Count;
	}

	// cumulative drawn tiles per cell row, so per-frame row slicing does not
	// rescan the whole map above a visible row
	void BuildRowPrefix()
	{
		const uint32_t Size = m_Height + 1;
		m_vRowPrefix.assign(Size, 0);
		for(uint32_t Cy = 0; Cy < m_ChunksY; ++Cy)
		{
			const uint32_t RowBase = Cy << CHUNK_POW;
			const uint32_t RowCount = std::min(CHUNK_SIZE, m_Height - RowBase);
			for(uint32_t Cx = 0; Cx < m_ChunksX; ++Cx)
			{
				const SChunk &Chunk = m_vChunks[ChunkId(Cx, Cy)];
				for(uint32_t Ky = 0; Ky < RowCount; ++Ky)
					m_vRowPrefix[RowBase + Ky + 1] += CountInChunk(Chunk, 0, Ky, CHUNK_SIZE, Ky + 1);
			}
		}
		for(uint32_t Y = 1; Y <= m_Height; ++Y)
			m_vRowPrefix[Y] += m_vRowPrefix[Y - 1];
	}

	// drawn tiles strictly before the start of row Y (i.e. rows 0..Y-1)
	uint32_t RowPrefix(uint32_t Y) const
	{
		if(Y > m_Height)
			Y = m_Height;
		return m_vRowPrefix[Y];
	}

	// drawn tiles in row Y at columns [0, X)
	uint32_t ColsBefore(uint32_t X, uint32_t Y) const
	{
		if(X > m_Width)
			X = m_Width;
		if(Y >= m_Height)
			return 0;
		const uint32_t Kx = X & X_MASK;
		const uint32_t Ky = Y & Y_MASK;
		uint32_t Count = 0;
		for(uint32_t C = 0; C < X >> CHUNK_POW; ++C)
			Count += CountInChunk(m_vChunks[ChunkId(C, Y >> CHUNK_POW)], 0, Ky, CHUNK_SIZE, Ky + 1);
		if(Kx > 0)
			Count += CountInChunk(m_vChunks[ChunkId(X >> CHUNK_POW, Y >> CHUNK_POW)], 0, Ky, Kx, Ky + 1);
		return Count;
	}

	// drawn tiles in row Y at columns [X0, X1); skips re-scanning columns before X0
	uint32_t RowExtras(uint32_t X0, uint32_t X1, uint32_t Y) const
	{
		if(X0 > m_Width)
			X0 = m_Width;
		if(X1 > m_Width)
			X1 = m_Width;
		if(X0 >= X1 || Y >= m_Height)
			return 0;
		const uint32_t Cy = Y >> CHUNK_POW;
		const uint32_t Ky = Y & Y_MASK;
		const uint32_t Cx0 = X0 >> CHUNK_POW;
		const uint32_t Cx1 = X1 >> CHUNK_POW;
		const uint32_t Kx0 = X0 & X_MASK;
		const uint32_t Kx1 = X1 & X_MASK;
		if(Cx0 == Cx1)
			return CountInChunk(m_vChunks[ChunkId(Cx0, Cy)], Kx0, Ky, Kx1, Ky + 1);
		uint32_t Count = CountInChunk(m_vChunks[ChunkId(Cx0, Cy)], Kx0, Ky, CHUNK_SIZE, Ky + 1);
		for(uint32_t C = Cx0 + 1; C < Cx1; ++C)
			Count += CountInChunk(m_vChunks[ChunkId(C, Cy)], 0, Ky, CHUNK_SIZE, Ky + 1);
		if(Kx1 > 0)
			Count += CountInChunk(m_vChunks[ChunkId(Cx1, Cy)], 0, Ky, Kx1, Ky + 1);
		return Count;
	}

private:
	static uint32_t CountInChunk(const SChunk &Chunk, uint32_t Kx0, uint32_t Ky0, uint32_t Kx1, uint32_t Ky1)
	{
		if(Kx0 >= Kx1 || Ky0 >= Ky1)
			return 0;
		if(Chunk.m_Container == CONTAINER_DENSE)
		{
			uint32_t Count = 0;
			for(uint32_t Y = Ky0; Y < Ky1; ++Y)
			{
				const uint16_t *pRow = &Chunk.m_aDense[Y * CHUNK_SIZE];
				for(uint32_t X = Kx0; X < Kx1; ++X)
					Count += pRow[X] != 0;
			}
			return Count;
		}
		if(Chunk.m_Container == CONTAINER_SPARSE)
		{
			const auto &vTiles = Chunk.m_aSparse;
			const auto Lower = [&](uint32_t Key) {
				return std::lower_bound(vTiles.begin(), vTiles.end(), Key, [](const SChunkTile &Tile, uint32_t TileKey) {
					return Tile.Y() * CHUNK_SIZE + Tile.X() < TileKey;
				});
			};
			const auto Upper = [&](uint32_t Key) {
				return std::upper_bound(vTiles.begin(), vTiles.end(), Key, [](uint32_t TileKey, const SChunkTile &Tile) {
					return TileKey < Tile.Y() * CHUNK_SIZE + Tile.X();
				});
			};
			// a range spanning whole rows is contiguous in row-major key order ...
			if(Kx0 == 0 && Kx1 == CHUNK_SIZE)
				return (uint32_t)(Upper((Ky1 - 1) * CHUNK_SIZE + (CHUNK_SIZE - 1)) - Lower(Ky0 * CHUNK_SIZE));
			// ... but a partial-row strip is not, so count row by row
			uint32_t Count = 0;
			for(uint32_t Y = Ky0; Y < Ky1; ++Y)
				Count += (uint32_t)(Upper(Y * CHUNK_SIZE + (Kx1 - 1)) - Lower(Y * CHUNK_SIZE + Kx0));
			return Count;
		}
		return 0;
	}

	uint32_t m_ChunksX = 0;
	uint32_t m_ChunksY = 0;
	uint32_t m_Width = 0;
	uint32_t m_Height = 0;
	std::vector<SChunk> m_vChunks;
	std::vector<uint32_t> m_vRowPrefix;
};

class CRenderLayer : public CRenderComponent
{
public:
	CRenderLayer(int GroupId, int LayerId, int Flags);
	virtual void OnInit(IGraphics *pGraphics, ITextRender *pTextRender, CRenderMap *pRenderMap, std::shared_ptr<CEnvelopeManager> &pEnvelopeManager, IMap *pMap, IMapImages *pMapImages, std::optional<FCallbackLayerInit> &CallbackLayerInitOptional);

	virtual void Init() = 0;
	virtual void Render(const CRenderLayerParams &Params) = 0;
	virtual bool DoRender(const CRenderLayerParams &Params) = 0;
	virtual bool IsValid() const { return true; }
	virtual bool IsGroup() const { return false; }
	virtual void Unload() = 0;

	bool IsVisibleInClipRegion(const std::optional<CClipRegion> &ClipRegion) const;
	int GetGroup() const { return m_GroupId; }

protected:
	int m_GroupId;
	int m_LayerId;
	int m_Flags;

	void UseTexture(IGraphics::CTextureHandle TextureHandle);
	virtual IGraphics::CTextureHandle GetTexture() const = 0;
	virtual void InitCallback() const;

	class IMap *m_pMap = nullptr;
	IMapImages *m_pMapImages = nullptr;
	std::shared_ptr<CEnvelopeManager> m_pEnvelopeManager;
	std::optional<FCallbackLayerInit> m_InitCallback;
	std::optional<CClipRegion> m_LayerClip;
};

class CRenderLayerGroup : public CRenderLayer
{
public:
	CRenderLayerGroup(int GroupId, CMapItemGroup *pGroup);
	~CRenderLayerGroup() override = default;
	void Init() override;
	void Render(const CRenderLayerParams &Params) override;
	bool DoRender(const CRenderLayerParams &Params) override;
	bool IsValid() const override { return m_pGroup != nullptr; }
	bool IsGroup() const override { return true; }
	void Unload() override {}
	void InitCallback() const override;

protected:
	IGraphics::CTextureHandle GetTexture() const override
	{
		return IGraphics::CTextureHandle();
	}

	CMapItemGroup *m_pGroup;
};

class CRenderLayerTile : public CRenderLayer
{
public:
	CRenderLayerTile(int GroupId, int LayerId, int Flags, CMapItemLayerTilemap *pLayerTilemap);
	~CRenderLayerTile() override = default;
	void Render(const CRenderLayerParams &Params) override;
	bool DoRender(const CRenderLayerParams &Params) override;
	void Init() override;
	void OnInit(IGraphics *pGraphics, ITextRender *pTextRender, CRenderMap *pRenderMap, std::shared_ptr<CEnvelopeManager> &pEnvelopeManager, IMap *pMap, IMapImages *pMapImages, std::optional<FCallbackLayerInit> &CallbackLayerInitOptional) override;

	virtual int GetDataIndex() const;
	bool IsValid() const override { return GetRawData() != nullptr; }
	void Unload() override;

protected:
	virtual void *GetRawData() const;
	template<class T>
	T *GetData() const;

	virtual ColorRGBA GetRenderColor(const CRenderLayerParams &Params) const;
	virtual void InitTileData();
	virtual void GetTileData(unsigned char *pIndex, unsigned char *pFlags, int *pAngleRotate, unsigned int x, unsigned int y, int CurOverlay) const;
	IGraphics::CTextureHandle GetTexture() const override { return m_TextureHandle; }
	CTile *m_pTiles;

private:
	IGraphics::CTextureHandle m_TextureHandle;

protected:
	class CTileLayerVisuals : public CRenderComponent
	{
	public:
		CTileLayerVisuals()
		{
			m_Width = 0;
			m_Height = 0;
			m_BufferContainerIndex = -1;
			m_IsTextured = false;
		}

		bool Init(unsigned int Width, unsigned int Height, bool AllocateTiles = true);
		void Unload();

		class CTileVisual
		{
		public:
			CTileVisual() :
				m_IndexBufferByteOffset(0) {}

		private:
			offset_ptr32 m_IndexBufferByteOffset;

		public:
			bool DoDraw() const
			{
				return (m_IndexBufferByteOffset & 0x10000000) != 0;
			}

			void Draw(bool SetDraw)
			{
				m_IndexBufferByteOffset = (SetDraw ? 0x10000000 : (offset_ptr32)0) | (m_IndexBufferByteOffset & 0xEFFFFFFF);
			}

			offset_ptr IndexBufferByteOffset() const
			{
				return ((offset_ptr)(m_IndexBufferByteOffset & 0xEFFFFFFF) * 6 * sizeof(uint32_t));
			}

			void SetIndexBufferByteOffset(offset_ptr32 IndexBufferByteOff)
			{
				m_IndexBufferByteOffset = IndexBufferByteOff | (m_IndexBufferByteOffset & 0x10000000);
			}

			void AddIndexBufferByteOffset(offset_ptr32 IndexBufferByteOff)
			{
				m_IndexBufferByteOffset = ((m_IndexBufferByteOffset & 0xEFFFFFFF) + IndexBufferByteOff) | (m_IndexBufferByteOffset & 0x10000000);
			}
		};

		std::vector<CTileVisual> m_vTilesOfLayer;

		CTileVisual m_BorderTopLeft;
		CTileVisual m_BorderTopRight;
		CTileVisual m_BorderBottomRight;
		CTileVisual m_BorderBottomLeft;

		CTileVisual m_BorderKillTile; // end of map kill tile -- game layer only

		std::vector<CTileVisual> m_vBorderTop;
		std::vector<CTileVisual> m_vBorderLeft;
		std::vector<CTileVisual> m_vBorderRight;
		std::vector<CTileVisual> m_vBorderBottom;

		unsigned int m_Width;
		unsigned int m_Height;
		int m_BufferContainerIndex;
		bool m_IsTextured;
	};

	void UploadTileData(std::optional<CTileLayerVisuals> &VisualsOptional, int CurOverlay, bool AddAsSpeedup, bool IsGameLayer = false);
	void UploadTileDataChunked(std::optional<CTileLayerVisuals> &VisualsOptional);
	void UploadTileDataBuffer(CTileLayerVisuals &Visuals, std::vector<CGraphicTile> &vTmpTiles, std::vector<CGraphicTileTextureCoords> &vTmpTileTexCoords, bool DoTextureCoords);

	virtual void RenderTileLayerWithTileBuffer(const ColorRGBA &Color, const CRenderLayerParams &Params);
	virtual void RenderTileLayerNoTileBuffer(const ColorRGBA &Color, const CRenderLayerParams &Params);

	void RenderTileLayer(const ColorRGBA &Color, const CRenderLayerParams &Params, CTileLayerVisuals *pTileLayerVisuals = nullptr);
	void RenderTileBorder(const ColorRGBA &Color, int BorderX0, int BorderY0, int BorderX1, int BorderY1, CTileLayerVisuals *pTileLayerVisuals);
	void RenderKillTileBorder(const ColorRGBA &Color);

	// reusable per-frame buffers for the sliced tile path, avoids per-frame heap allocations
	std::vector<offset_ptr_size> m_vIndexOffsets;
	std::vector<unsigned int> m_vDrawCounts;

	std::optional<CRenderLayerTile::CTileLayerVisuals> m_VisualTiles;
	CMapItemLayerTilemap *m_pLayerTilemap;
	ColorRGBA m_Color;
	bool m_IsChunkable;
	std::optional<CChunkTileLayer> m_ChunkTiles;
};

class CRenderLayerQuads : public CRenderLayer
{
public:
	CRenderLayerQuads(int GroupId, int LayerId, int Flags, CMapItemLayerQuads *pLayerQuads);
	void OnInit(IGraphics *pGraphics, ITextRender *pTextRender, CRenderMap *pRenderMap, std::shared_ptr<CEnvelopeManager> &pEnvelopeManager, IMap *pMap, IMapImages *pMapImages, std::optional<FCallbackLayerInit> &CallbackLayerInitOptional) override;
	void Init() override;
	bool IsValid() const override { return m_pLayerQuads->m_NumQuads > 0 && m_pQuads; }
	void Render(const CRenderLayerParams &Params) override;
	bool DoRender(const CRenderLayerParams &Params) override;
	void Unload() override;

protected:
	IGraphics::CTextureHandle GetTexture() const override { return m_TextureHandle; }

	class CQuadLayerVisuals : public CRenderComponent
	{
	public:
		CQuadLayerVisuals() :
			m_QuadNum(0), m_BufferContainerIndex(-1), m_IsTextured(false) {}
		void Unload();

		int m_QuadNum;
		int m_BufferContainerIndex;
		bool m_IsTextured;
	};
	void RenderQuadLayer(float Alpha, const CRenderLayerParams &Params);

	std::optional<CRenderLayerQuads::CQuadLayerVisuals> m_VisualQuad;
	CMapItemLayerQuads *m_pLayerQuads;

	class CQuadCluster
	{
	public:
		bool m_Grouped;
		int m_StartIndex;
		int m_NumQuads;

		int m_PosEnv;
		float m_PosEnvOffset;
		int m_ColorEnv;
		float m_ColorEnvOffset;

		std::vector<SQuadRenderInfo> m_vQuadRenderInfo;
		std::optional<CClipRegion> m_ClipRegion;
		std::optional<CClipRegion> m_StaticBounds; // bounds without envelope
	};
	void CalculateClipping(CQuadCluster &QuadCluster);
	void CalculateStaticBounds(CQuadCluster &QuadCluster);
	bool CalculateQuadClipping(const CQuadCluster &QuadCluster, float aQuadOffsetMin[2], float aQuadOffsetMax[2]) const;

	std::vector<CQuadCluster> m_vQuadClusters;
	CQuad *m_pQuads;

private:
	IGraphics::CTextureHandle m_TextureHandle;
};

class CRenderLayerEntityBase : public CRenderLayerTile
{
public:
	CRenderLayerEntityBase(int GroupId, int LayerId, int Flags, CMapItemLayerTilemap *pLayerTilemap);
	~CRenderLayerEntityBase() override = default;
	bool DoRender(const CRenderLayerParams &Params) override;

protected:
	ColorRGBA GetRenderColor(const CRenderLayerParams &Params) const override { return ColorRGBA(1.0f, 1.0f, 1.0f, Params.m_EntityOverlayVal / 100.0f); }
	IGraphics::CTextureHandle GetTexture() const override;
};

class CRenderLayerEntityGame final : public CRenderLayerEntityBase
{
public:
	CRenderLayerEntityGame(int GroupId, int LayerId, int Flags, CMapItemLayerTilemap *pLayerTilemap);
	void Init() override;

protected:
	void RenderTileLayerWithTileBuffer(const ColorRGBA &Color, const CRenderLayerParams &Params) override;
	void RenderTileLayerNoTileBuffer(const ColorRGBA &Color, const CRenderLayerParams &Params) override;

private:
	ColorRGBA GetDeathBorderColor() const;
};

class CRenderLayerEntityFront final : public CRenderLayerEntityBase
{
public:
	CRenderLayerEntityFront(int GroupId, int LayerId, int Flags, CMapItemLayerTilemap *pLayerTilemap);
	int GetDataIndex() const override;
};

class CRenderLayerEntityTele final : public CRenderLayerEntityBase
{
public:
	CRenderLayerEntityTele(int GroupId, int LayerId, int Flags, CMapItemLayerTilemap *pLayerTilemap);
	int GetDataIndex() const override;
	void Init() override;
	void InitTileData() override;
	void Unload() override;

protected:
	void RenderTileLayerWithTileBuffer(const ColorRGBA &Color, const CRenderLayerParams &Params) override;
	void RenderTileLayerNoTileBuffer(const ColorRGBA &Color, const CRenderLayerParams &Params) override;
	void GetTileData(unsigned char *pIndex, unsigned char *pFlags, int *pAngleRotate, unsigned int x, unsigned int y, int CurOverlay) const override;

private:
	std::optional<CRenderLayerTile::CTileLayerVisuals> m_VisualTeleNumbers;
	CTeleTile *m_pTeleTiles;
};

class CRenderLayerEntitySpeedup final : public CRenderLayerEntityBase
{
public:
	CRenderLayerEntitySpeedup(int GroupId, int LayerId, int Flags, CMapItemLayerTilemap *pLayerTilemap);
	int GetDataIndex() const override;
	void Init() override;
	void InitTileData() override;
	void Unload() override;

protected:
	void RenderTileLayerWithTileBuffer(const ColorRGBA &Color, const CRenderLayerParams &Params) override;
	void RenderTileLayerNoTileBuffer(const ColorRGBA &Color, const CRenderLayerParams &Params) override;
	void GetTileData(unsigned char *pIndex, unsigned char *pFlags, int *pAngleRotate, unsigned int x, unsigned int y, int CurOverlay) const override;
	IGraphics::CTextureHandle GetTexture() const override;

private:
	std::optional<CRenderLayerTile::CTileLayerVisuals> m_VisualForce;
	std::optional<CRenderLayerTile::CTileLayerVisuals> m_VisualMaxSpeed;
	CSpeedupTile *m_pSpeedupTiles;
};

class CRenderLayerEntitySwitch final : public CRenderLayerEntityBase
{
public:
	CRenderLayerEntitySwitch(int GroupId, int LayerId, int Flags, CMapItemLayerTilemap *pLayerTilemap);
	int GetDataIndex() const override;
	void Init() override;
	void InitTileData() override;
	void Unload() override;

protected:
	void RenderTileLayerWithTileBuffer(const ColorRGBA &Color, const CRenderLayerParams &Params) override;
	void RenderTileLayerNoTileBuffer(const ColorRGBA &Color, const CRenderLayerParams &Params) override;
	void GetTileData(unsigned char *pIndex, unsigned char *pFlags, int *pAngleRotate, unsigned int x, unsigned int y, int CurOverlay) const override;
	IGraphics::CTextureHandle GetTexture() const override;

private:
	std::optional<CRenderLayerTile::CTileLayerVisuals> m_VisualSwitchNumberTop;
	std::optional<CRenderLayerTile::CTileLayerVisuals> m_VisualSwitchNumberBottom;
	CSwitchTile *m_pSwitchTiles;
};

class CRenderLayerEntityTune final : public CRenderLayerEntityBase
{
public:
	CRenderLayerEntityTune(int GroupId, int LayerId, int Flags, CMapItemLayerTilemap *pLayerTilemap);
	int GetDataIndex() const override;
	void Init() override;
	void InitTileData() override;

protected:
	void RenderTileLayerNoTileBuffer(const ColorRGBA &Color, const CRenderLayerParams &Params) override;
	void GetTileData(unsigned char *pIndex, unsigned char *pFlags, int *pAngleRotate, unsigned int x, unsigned int y, int CurOverlay) const override;
	IGraphics::CTextureHandle GetTexture() const override;

private:
	CTuneTile *m_pTuneTiles;
	mutable CTuneColorMapper m_TuneColorMapper;
};
#endif
