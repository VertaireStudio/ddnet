#include <engine/gfx/image_loader.h>

#include <gtest/gtest.h>

#include <cstring>

static CImageInfo CreateTestImage(int Width, int Height)
{
	CImageInfo Image;
	Image.m_Width = Width;
	Image.m_Height = Height;
	Image.m_Format = CImageInfo::FORMAT_RGBA;
	Image.Allocate();
	for(int y = 0; y < Height; ++y)
	{
		for(int x = 0; x < Width; ++x)
		{
			const size_t Offset = (size_t)(y * Width + x) * 4;
			Image.m_pData[Offset + 0] = (uint8_t)((x * 40 + y * 7) % 256);
			Image.m_pData[Offset + 1] = (uint8_t)((y * 40 + x * 7) % 256);
			Image.m_pData[Offset + 2] = (uint8_t)((x * 11 + y * 13) % 256);
			Image.m_pData[Offset + 3] = (uint8_t)(x == 0 ? 0 : 255 - y * 63);
		}
	}
	return Image;
}

TEST(ImageLoader, DetectFormat)
{
	EXPECT_EQ(CImageLoader::DetectImageFormat((const uint8_t *)"\x89PNG\r\n\x1a\nxyz", 8), CImageLoader::FORMAT_PNG);
	EXPECT_EQ(CImageLoader::DetectImageFormat((const uint8_t *)"garbage", 7), CImageLoader::FORMAT_NONE);
#ifdef CONF_WEBP
	EXPECT_EQ(CImageLoader::DetectImageFormat((const uint8_t *)"RIFF\x00\x00\x00\x00WEBP", 12), CImageLoader::FORMAT_WEBP);
#endif
}

TEST(ImageLoader, SavePngLoadPngRoundtrip)
{
	CImageInfo Image = CreateTestImage(8, 8);
	CByteBufferWriter Writer;
	ASSERT_TRUE(CImageLoader::SavePng(Writer, Image));
	ASSERT_GT(Writer.Size(), 0);
	EXPECT_EQ(CImageLoader::DetectImageFormat(Writer.Data(), Writer.Size()), CImageLoader::FORMAT_PNG);

	CByteBufferReader Reader(Writer.Data(), Writer.Size());
	CImageInfo Loaded;
	int LoadedError = 0;
	ASSERT_TRUE(CImageLoader::LoadPng(Reader, "test", Loaded, LoadedError));
	EXPECT_EQ(LoadedError, 0);
	EXPECT_TRUE(Loaded.DataEquals(Image));
}

#ifdef CONF_WEBP
TEST(ImageLoader, SaveWebpLoadWebpRoundtrip)
{
	CImageInfo Image = CreateTestImage(8, 8);
	CByteBufferWriter Writer;
	ASSERT_TRUE(CImageLoader::SaveWebp(Writer, Image));
	ASSERT_GT(Writer.Size(), 0);
	EXPECT_EQ(CImageLoader::DetectImageFormat(Writer.Data(), Writer.Size()), CImageLoader::FORMAT_WEBP);

	CImageInfo Loaded;
	ASSERT_TRUE(CImageLoader::LoadWebp(Writer.Data(), Writer.Size(), "test", Loaded));
	EXPECT_TRUE(Loaded.DataEquals(Image));
}

TEST(ImageLoader, LoadImageDetectsWebp)
{
	CImageInfo Image = CreateTestImage(8, 8);
	CByteBufferWriter Writer;
	ASSERT_TRUE(CImageLoader::SaveWebp(Writer, Image));

	CImageInfo Loaded;
	int LoadedError = 0;
	ASSERT_TRUE(CImageLoader::LoadImage(Writer.Data(), Writer.Size(), "test.webp", Loaded, LoadedError));
	EXPECT_TRUE(Loaded.DataEquals(Image));
}

TEST(ImageLoader, SaveWebpLossyRoundtrip)
{
	CImageInfo Image = CreateTestImage(8, 8);
	CByteBufferWriter Writer;
	ASSERT_TRUE(CImageLoader::SaveWebpLossy(Writer, Image, 90));
	ASSERT_GT(Writer.Size(), 0);
	EXPECT_EQ(CImageLoader::DetectImageFormat(Writer.Data(), Writer.Size()), CImageLoader::FORMAT_WEBP);
	EXPECT_TRUE(CImageLoader::IsLossyWebp(Writer.Data(), Writer.Size()));

	CImageInfo Loaded;
	ASSERT_TRUE(CImageLoader::LoadWebp(Writer.Data(), Writer.Size(), "test", Loaded));
	EXPECT_EQ(Loaded.m_Width, Image.m_Width);
	EXPECT_EQ(Loaded.m_Height, Image.m_Height);
}

TEST(ImageLoader, IsLossyWebpDetection)
{
	EXPECT_FALSE(CImageLoader::IsLossyWebp(nullptr, 0));
	EXPECT_FALSE(CImageLoader::IsLossyWebp((const uint8_t *)"garbage", 7));
	EXPECT_FALSE(CImageLoader::IsLossyWebp((const uint8_t *)"RIFF\x00\x00\x00\x00WEBP", 12));

	CImageInfo Image = CreateTestImage(8, 8);

	CByteBufferWriter LosslessWriter;
	ASSERT_TRUE(CImageLoader::SaveWebp(LosslessWriter, Image));
	EXPECT_FALSE(CImageLoader::IsLossyWebp(LosslessWriter.Data(), LosslessWriter.Size()));

	CByteBufferWriter LossyWriter;
	ASSERT_TRUE(CImageLoader::SaveWebpLossy(LossyWriter, Image, 50));
	EXPECT_TRUE(CImageLoader::IsLossyWebp(LossyWriter.Data(), LossyWriter.Size()));
}
#endif