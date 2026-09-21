/* (c) DDNet developers. See licence.txt in the root of the distribution for more information. */

#include <base/fs.h>
#include <base/logger.h>
#include <base/os.h>
#include <base/str.h>

#include <engine/gfx/image_loader.h>
#include <engine/shared/datafile.h>
#include <engine/storage.h>

#include <game/mapitems.h>

#include <map>
#include <memory>

static const char *TOOL_NAME = "map_convert_imageformat";

/*
	Usage: map_convert_imageformat <source map> <absolute destination directory> [quality]
	Converts the embedded RGBA images of a map to WebP and writes the exported map
	to '<absolute destination directory>/<source map basename>'.
	Images that are already embedded as WebP are passed through verbatim.
	Quality must be omitted or 0 for lossless WebP, or given as 1-100 for lossy WebP.
*/

static int ConvertMap(const char *pSourceMap, const char *pDestinationDir, int Quality, IStorage *pStorage)
{
#ifdef CONF_WEBP
	CDataFileReader Reader;
	if(!Reader.Open(pStorage, pSourceMap, IStorage::TYPE_ABSOLUTE))
	{
		log_error(TOOL_NAME, "Failed to open source map '%s' for reading", pSourceMap);
		return -1;
	}

	char aDestinationDir[IO_MAX_PATH_LENGTH];
	str_copy(aDestinationDir, pDestinationDir, sizeof(aDestinationDir));
	if(fs_makedir_rec_for(aDestinationDir) != 0 || fs_makedir(aDestinationDir) != 0)
	{
		log_error(TOOL_NAME, "Failed to create destination directory '%s'", aDestinationDir);
		Reader.Close();
		return -1;
	}
	char aDestinationMap[IO_MAX_PATH_LENGTH];
	str_format(aDestinationMap, sizeof(aDestinationMap), "%s/%s", aDestinationDir, fs_filename(pSourceMap));

	CDataFileWriter Writer;
	if(!Writer.Open(pStorage, aDestinationMap, IStorage::TYPE_ABSOLUTE))
	{
		log_error(TOOL_NAME, "Failed to open destination map '%s' for writing", aDestinationMap);
		Reader.Close();
		return -1;
	}

	// data indices whose content is replaced by converted WebP bytes (index order is preserved)
	std::map<int, std::vector<uint8_t>> vWebpData;
	int NumConverted = 0;

	// add all items
	for(int Index = 0; Index < Reader.NumItems(); Index++)
	{
		int Type, Id;
		CUuid Uuid;
		void *pItem = Reader.GetItem(Index, &Type, &Id, &Uuid);

		// Filter ITEMTYPE_EX items, they will be automatically added again.
		if(Type == ITEMTYPE_EX)
		{
			continue;
		}

		int Size = Reader.GetItemSize(Index);

		if(Type == MAPITEMTYPE_IMAGE)
		{
			const CMapItemImage *pImgItem = static_cast<CMapItemImage *>(pItem);
			const bool IsV2 = pImgItem->m_Version >= 2 && Size >= (int)sizeof(CMapItemImage_v2);
			const int MustBe1 = IsV2 ? static_cast<CMapItemImage_v2 *>(pItem)->m_MustBe1 : CMapItemImageFormat::RGBA;

			if(pImgItem->m_External)
			{
				log_info(TOOL_NAME, "keeping external image '%s' as-is", Reader.GetDataString(pImgItem->m_ImageName) == nullptr ? "?" : Reader.GetDataString(pImgItem->m_ImageName));
			}
			else if(MustBe1 == CMapItemImageFormat::WEBP)
			{
				// already embedded as WebP, pass through untouched
			}
			else
			{
				if(MustBe1 != CMapItemImageFormat::RGBA)
				{
					log_error(TOOL_NAME, "ignoring image %d with unknown format %d", Index, MustBe1);
				}
				else
				{
					const char *pName = Reader.GetDataString(pImgItem->m_ImageName);
					const int DataId = pImgItem->m_ImageData;
					const int DataSize = Reader.GetDataSize(DataId);
					const size_t ExpectedSize = (size_t)pImgItem->m_Width * pImgItem->m_Height * 4;
					if(pName == nullptr)
					{
						log_error(TOOL_NAME, "failed to load name of image %d", Index);
					}
					else if(DataId < 0 || DataId >= Reader.NumData() || DataSize != (int)ExpectedSize)
					{
						log_error(TOOL_NAME, "invalid RGBA data size %d for image '%s', expected %d", DataSize, pName, ExpectedSize);
					}
					else
					{
						CImageInfo Image;
						Image.m_Width = pImgItem->m_Width;
						Image.m_Height = pImgItem->m_Height;
						Image.m_Format = CImageInfo::FORMAT_RGBA;
						Image.m_pData = static_cast<uint8_t *>(Reader.GetData(DataId));

						CByteBufferWriter EncodedImage;
						const bool Encoded = Quality > 0 ? CImageLoader::SaveWebpLossy(EncodedImage, Image, Quality) : CImageLoader::SaveWebp(EncodedImage, Image);
						if(!Encoded)
						{
							log_error(TOOL_NAME, "failed to encode image '%s' as WebP, keeping it as-is", pName);
						}
						else
						{
							CMapItemImage_v2 NewImageItem = {};
							NewImageItem.m_Version = 2;
							NewImageItem.m_MustBe1 = CMapItemImageFormat::WEBP;
							NewImageItem.m_Width = pImgItem->m_Width;
							NewImageItem.m_Height = pImgItem->m_Height;
							NewImageItem.m_External = 0;
							NewImageItem.m_ImageName = pImgItem->m_ImageName;
							NewImageItem.m_ImageData = pImgItem->m_ImageData;
							pItem = &NewImageItem;
							Size = sizeof(NewImageItem);

							vWebpData[DataId] = {EncodedImage.Data(), EncodedImage.Data() + EncodedImage.Size()};
							NumConverted++;
							log_info(TOOL_NAME, "converted image '%s': RGBA %dx%d (%d B) -> %s WebP (%u B)", pName, pImgItem->m_Width, pImgItem->m_Height, DataSize, Quality > 0 ? "lossy" : "lossless", (unsigned)EncodedImage.Size());
						}
					}
				}
			}
		}

		Writer.AddItem(Type, Id, Size, pItem, &Uuid);
	}

	// add all data
	for(int Index = 0; Index < Reader.NumData(); Index++)
	{
		auto Found = vWebpData.find(Index);
		if(Found != vWebpData.end())
		{
			Writer.AddData(Found->second.size(), Found->second.data());
		}
		else
		{
			Writer.AddData(Reader.GetDataSize(Index), Reader.GetData(Index));
		}
	}

	Reader.Close();
	Writer.Finish();
	log_info(TOOL_NAME, "Converted '%s' (%d image%s) and wrote '%s'", pSourceMap, NumConverted, NumConverted == 1 ? "" : "s", aDestinationMap);
	return 0;
#else
	(void)pSourceMap;
	(void)pDestinationDir;
	(void)Quality;
	(void)pStorage;
	log_error(TOOL_NAME, "This tool requires a WebP build (rebuild with WebP support enabled)");
	return -1;
#endif
}

int main(int argc, const char **argv)
{
	CCmdlineFix CmdlineFix(&argc, &argv);
	log_set_global_logger_default();

	if(argc < 3 || argc > 4)
	{
		log_error(TOOL_NAME, "Usage: %s <source map> <absolute destination directory> [quality]", TOOL_NAME);
		return -1;
	}

	const int Quality = argc == 4 ? str_toint(argv[3]) : 0;
	if(Quality < 0 || Quality > 100)
	{
		log_error(TOOL_NAME, "Quality must be between 0 and 100");
		return -1;
	}

	std::unique_ptr<IStorage> pStorage = std::unique_ptr<IStorage>(CreateStorage(IStorage::EInitializationType::BASIC, argc, argv));
	if(!pStorage)
	{
		log_error(TOOL_NAME, "Error creating basic storage");
		return -1;
	}

	return ConvertMap(argv[1], argv[2], Quality, pStorage.get());
}