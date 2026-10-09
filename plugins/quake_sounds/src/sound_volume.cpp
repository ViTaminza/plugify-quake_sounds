// quake_sounds — sound_volume over s2sdk user messages (see sound_volume.hpp).
#include "sound_volume.hpp"

#include <string>

#include <plg/string.hpp>
#include <s2sdk/s2sdk.hpp>

#include "quake_core.hpp"

namespace sound_volume {

namespace {

// EBaseGameEvents of CS2 (ref/src/cs2-lvl_ranks_modules/LR_FakeRanks/protobuf/gameevents.proto).
constexpr int kSosStartFallback = 208;
constexpr int kSosStopFallback = 209;
constexpr int kSosSetParamsFallback = 210;

int g_iStartId = -1;
int g_iSetId = -1;
int g_iStopId = -1;
bool g_bAvailable = false;

int ResolveId(const char* szName, int iFallback)
{
	const int iId = s2sdk::UserMessageFindMessageIdByName(plg::string(szName));
	return iId > 0 ? iId : iFallback;
}

// One parameter-set message to a single slot. The recipients are a mask so the sound only has to
// be started once; the original did the same (one PostEventAbstract per recipient).
void SendParamsTo(int iSlot, int iGuid, float flVolume)
{
	if (!g_bAvailable || g_iSetId <= 0 || iSlot < 0 || iSlot >= 64) return;

	void* pMsg = s2sdk::UserMessageCreateFromId((int16_t)g_iSetId);
	if (!pMsg) return;

	s2sdk::PbSetInt32(pMsg, "soundevent_guid", iGuid);
	s2sdk::PbSetString(pMsg, "packed_params", plg::string(qs::PackVolumeParam(flVolume)));
	s2sdk::UserMessageSetRecipientMask(pMsg, 1ull << iSlot);
	s2sdk::UserMessageSend(pMsg);
	s2sdk::UserMessageDestroy(pMsg);
}

} // namespace

bool Init()
{
	// The generated wrappers call these globals; in a process without the s2sdk plugin they are
	// null. Check the exact set this module uses.
	g_bAvailable = __s2sdk_UserMessageFindMessageIdByName && __s2sdk_UserMessageCreateFromId
		&& __s2sdk_UserMessageSend && __s2sdk_UserMessageDestroy && __s2sdk_UserMessageGetMessageID
		&& __s2sdk_UserMessageGetRecipientMask && __s2sdk_UserMessageSetRecipientMask
		&& __s2sdk_PbReadInt32 && __s2sdk_PbReadUInt32 && __s2sdk_PbSetInt32 && __s2sdk_PbSetString
		&& __s2sdk_UserMessageHasField && __s2sdk_HookUserMessage;
	if (!g_bAvailable) return false;

	g_iStartId = ResolveId("GE_SosStartSoundEvent", kSosStartFallback);
	g_iStopId = ResolveId("GE_SosStopSoundEvent", kSosStopFallback);
	g_iSetId = ResolveId("GE_SosSetSoundEventParams", kSosSetParamsFallback);
	return true;
}

int StartMessageId() { return g_iStartId; }
int SetMessageId() { return g_iSetId; }
int StopMessageId() { return g_iStopId; }

bool ParseStart(void* pUserMessage, int& iGuid, uint32_t& uHash)
{
	iGuid = -1;
	uHash = 0;
	if (!pUserMessage) return false;

	if (s2sdk::UserMessageGetMessageID(pUserMessage) != (int16_t)g_iStartId) return false;

	// soundevent_guid / soundevent_hash are optional scalars of CMsgSosStartSoundEvent: the last
	// argument of PbRead* is the *repeated* index (-1 for non-repeated fields). With an index >= 0
	// the read fails (s2sdk logs "Field ... is not repeated as expected" on every sound event) and
	// returns 0, which used to be taken for a valid guid.
	if (!s2sdk::UserMessageHasField(pUserMessage, "soundevent_guid")) return false;

	iGuid = s2sdk::PbReadInt32(pUserMessage, "soundevent_guid", -1);
	uHash = s2sdk::PbReadUInt32(pUserMessage, "soundevent_hash", -1);
	return iGuid >= 0;
}

uint64_t RecipientMask(void* pUserMessage)
{
	if (!pUserMessage) return 0;
	return s2sdk::UserMessageGetRecipientMask(pUserMessage);
}

void Set(int iSlot, int iGuid, float flVolume)
{
	SendParamsTo(iSlot, iGuid, flVolume);
}

void Stop(int iSlot, int iGuid)
{
	if (!g_bAvailable || g_iStopId <= 0 || iSlot < 0 || iSlot >= 64) return;

	void* pMsg = s2sdk::UserMessageCreateFromId((int16_t)g_iStopId);
	if (!pMsg) return;

	s2sdk::PbSetInt32(pMsg, "soundevent_guid", iGuid);
	s2sdk::UserMessageSetRecipientMask(pMsg, 1ull << iSlot);
	s2sdk::UserMessageSend(pMsg);
	s2sdk::UserMessageDestroy(pMsg);
}

} // namespace sound_volume
