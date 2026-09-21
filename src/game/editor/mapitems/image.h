#ifndef GAME_EDITOR_MAPITEMS_IMAGE_H
#define GAME_EDITOR_MAPITEMS_IMAGE_H

#include <base/types.h>

#include <engine/graphics.h>

#include <game/editor/auto_map.h>
#include <game/editor/map_object.h>

#include <vector>

class CEditorImage : public CImageInfo, public CMapObject
{
public:
	explicit CEditorImage(CEditorMap *pMap);
	~CEditorImage() override;
	void OnAttach(CEditorMap *pMap) override;

	void AnalyseTileFlags();
	void Free();
	bool CanEmbedWebpSource() const;

	CEditorImage &operator=(CImageInfo &&Other);

	IGraphics::CTextureHandle m_Texture;
	int m_External = 0;
	char m_aName[IO_MAX_PATH_LENGTH] = "";
	std::vector<uint8_t> m_WebpSourceData; // raw source WebP bytes, embedded verbatim on save while it matches the current pixels
	unsigned char m_aTileFlags[256];

	CAutomapper m_Automapper;
};

#endif
