#ifndef GAME_MAP_ENVELOPE_MANAGER_H
#define GAME_MAP_ENVELOPE_MANAGER_H

#include <engine/map.h>

#include <game/map/envelope_extrema.h>
#include <game/map/render_interfaces.h>

#include <memory>
#include <unordered_map>

class CEnvelopeManager
{
public:
	CEnvelopeManager(const IEnvelopeEval *pEnvelopeEval, IMap *pMap) :
		m_pEnvelopeEval(pEnvelopeEval), m_EnvelopeExtrema(pMap) {}

	const IEnvelopeEval *EnvelopeEval() const { return m_pEnvelopeEval; }
	const CEnvelopeExtrema *EnvelopeExtrema() const { return &m_EnvelopeExtrema; }

	void BeginFrame()
	{
		m_FrameCache.clear();
	}

	void CachedEnvelopeEval(int TimeOffsetMillis, int EnvelopeIndex, ColorRGBA &Result, size_t Channels) const
	{
		// cache key: combine index, time offset, and channels
		uint64_t Key = (uint64_t(EnvelopeIndex) << 32) | (uint64_t(TimeOffsetMillis) << 8) | (Channels & 0xFF);
		auto It = m_FrameCache.find(Key);
		if(It != m_FrameCache.end())
		{
			Result = It->second;
			return;
		}
		m_pEnvelopeEval->EnvelopeEval(TimeOffsetMillis, EnvelopeIndex, Result, Channels);
		m_FrameCache.emplace(Key, Result);
	}

private:
	const IEnvelopeEval *m_pEnvelopeEval;
	CEnvelopeExtrema m_EnvelopeExtrema;
	mutable std::unordered_map<uint64_t, ColorRGBA> m_FrameCache;
};

#endif
