#ifndef ENGINE_GFX_IMAGE_LOADER_H
#define ENGINE_GFX_IMAGE_LOADER_H

#include <base/types.h>

#include <engine/image.h>

#include <vector>

class CByteBufferReader
{
	const uint8_t *m_pData;
	size_t m_Size;
	size_t m_ReadOffset = 0;
	bool m_Error = false;

public:
	CByteBufferReader(const uint8_t *pData, size_t Size) :
		m_pData(pData),
		m_Size(Size) {}

	bool Read(void *pData, size_t Size);
	bool Error() const { return m_Error; }
};

class CByteBufferWriter
{
	std::vector<uint8_t> m_vBuffer;

public:
	void Write(const void *pData, size_t Size);
	const uint8_t *Data() const { return m_vBuffer.data(); }
	size_t Size() const { return m_vBuffer.size(); }
};

class CImageLoader
{
public:
	CImageLoader() = delete;

	enum
	{
		PNGLITE_COLOR_TYPE = 1 << 0,
		PNGLITE_BIT_DEPTH = 1 << 1,
		PNGLITE_INTERLACE_TYPE = 1 << 2,
		PNGLITE_COMPRESSION_TYPE = 1 << 3,
		PNGLITE_FILTER_TYPE = 1 << 4,
	};

	static bool LoadPng(CByteBufferReader &Reader, const char *pContextName, CImageInfo &Image, int &PngliteIncompatible);
	static bool LoadPng(IOHANDLE File, const char *pFilename, CImageInfo &Image, int &PngliteIncompatible);

	static bool SavePng(CByteBufferWriter &Writer, const CImageInfo &Image);
	static bool SavePng(IOHANDLE File, const char *pFilename, const CImageInfo &Image);

#ifdef CONF_WEBP
	static bool LoadWebp(const uint8_t *pData, size_t Size, const char *pContextName, CImageInfo &Image);
	static bool LoadWebp(IOHANDLE File, const char *pFilename, CImageInfo &Image);

	static bool SaveWebp(CByteBufferWriter &Writer, const CImageInfo &Image);
	static bool SaveWebp(IOHANDLE File, const char *pFilename, const CImageInfo &Image);

	/**
	 * Saves an image as a lossy WebP.
	 *
	 * @Writer The buffer that receives the encoded WebP data.
	 * @Image The image to encode, in `FORMAT_RGB` or `FORMAT_RGBA` format.
	 * @Quality The lossy encoding quality, from 0 (worst) to 100 (best).
	 *
	 * @return True on success.
	 */
	static bool SaveWebpLossy(CByteBufferWriter &Writer, const CImageInfo &Image, int Quality);

	/**
	 * Detects whether WebP data is lossy (VP8) rather than lossless (VP8L).
	 *
	 * @pData WebP data.
	 * @Size Size of the WebP data.
	 *
	 * @return True if the data is a lossy WebP, false for lossless WebP or unrecognized data.
	 */
	static bool IsLossyWebp(const uint8_t *pData, size_t Size);

	static bool SaveWebpLossy(IOHANDLE File, const char *pFilename, const CImageInfo &Image, int Quality);
#endif

	enum EDetectedImageFormat
	{
		FORMAT_NONE,
		FORMAT_PNG,
		FORMAT_WEBP,
	};

	/**
	 * Detects the format of an image from its magic bytes.
	 *
	 * @pData Image data.
	 * @Size Size of the image data.
	 *
	 * @return The detected image format, or `FORMAT_NONE` if the format is unrecognized.
	 */
	static EDetectedImageFormat DetectImageFormat(const uint8_t *pData, size_t Size);

	static bool LoadImage(const uint8_t *pData, size_t Size, const char *pContextName, CImageInfo &Image, int &PngliteIncompatible);
	static bool LoadImage(IOHANDLE File, const char *pFilename, CImageInfo &Image, int &PngliteIncompatible);

	static bool SaveImage(CByteBufferWriter &Writer, const char *pFilename, const CImageInfo &Image);
	static bool SaveImage(IOHANDLE File, const char *pFilename, const CImageInfo &Image);
};

#endif // ENGINE_GFX_IMAGE_LOADER_H
