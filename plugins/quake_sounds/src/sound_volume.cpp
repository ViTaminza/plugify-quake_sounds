// quake_sounds — sound_volume over s2sdk user messages (see sound_volume.hpp).
#include "sound_volume.hpp"

#include <algorithm>
#include <cctype>
#include <string>

#include <plg/string.hpp>
#include <s2sdk/s2sdk.hpp>

#include "quake_core.hpp"

namespace sound_volume {
namespace {

int g_iStartId = -1;
int g_iSetId = -1;
int g_iStopId = -1;
bool g_bAvailable = false;

int FindOne(const char* szName)
{
	if (!__s2sdk_UserMessageFindMessageIdByName) return 0;
	try
	{
		const int iId = (int)s2sdk::UserMessageFindMessageIdByName(plg::string(szName));
		return iId > 0 ? iId : 0;
	}
	catch (...)
	{
		return 0;
	}
}

std::string LowerName(const plg::string& name)
{
	std::string out(name.data(), name.size());
	std::transform(out.begin(), out.end(), out.begin(),
				   [](unsigned char c) { return (char)std::tolower(c); });
	return out;
}

// The incoming message is engine-owned: its serializable object is valid, so GetMessageName and
// GetMessageID are safe here. They are checked BEFORE any field read (HasField/PbRead).
bool IsStartMessage(void* pUserMessage)
{
	if (__s2sdk_UserMessageGetMessageName == nullptr || __s2sdk_UserMessageGetMessageID == nullptr ||
		__s2sdk_UserMessageGetSerializableMessage == nullptr)
		return false;
	if (s2sdk::UserMessageGetSerializableMessage(pUserMessage) == nullptr) return false;
	plg::string name;
	try { name = s2sdk::UserMessageGetMessageName(pUserMessage); } catch (...) { return false; }
	if (LowerName(name).find("sosstartsoundevent") == std::string::npos) return false;
	try { return (int)s2sdk::UserMessageGetMessageID(pUserMessage) == g_iStartId; } catch (...) { return false; }
}

// One parameter-set message to a single slot. The id comes from the registry lookup and was
// resolved synchronously on the live map; a newly created UserMessage with that id is valid.
// Nothing here calls GetMessageName/GetMessageID on the new object (unknown ids would return a
// non-null object with a null serializable and those getters dereference it).
void SendParamsTo(int iSlot, int iGuid, float flVolume)
{
	if (!g_bAvailable || g_iSetId <= 0 || iSlot < 0 || iSlot >= 64) return;

	void* pMsg = s2sdk::UserMessageCreateFromId((int16_t)g_iSetId);
	if (!pMsg) return;
	// A safe pointer getter (it does not dereference the serializable): a resolved id should
	// always have one, but never run Pb*/Send on an object without it.
	if (s2sdk::UserMessageGetSerializableMessage(pMsg) == nullptr)
	{
		s2sdk::UserMessageDestroy(pMsg);
		return;
	}

	try
	{
		s2sdk::PbSetInt32(pMsg, "soundevent_guid", iGuid);
		s2sdk::PbSetString(pMsg, "packed_params", plg::string(qs::PackVolumeParam(flVolume)));
		s2sdk::UserMessageSetRecipientMask(pMsg, 1ull << iSlot);
		s2sdk::UserMessageSend(pMsg);
	}
	catch (...)
	{
	}
	s2sdk::UserMessageDestroy(pMsg);
}

void SendStopTo(int iSlot, int iGuid)
{
	if (!g_bAvailable || g_iStopId <= 0 || iSlot < 0 || iSlot >= 64) return;

	void* pMsg = s2sdk::UserMessageCreateFromId((int16_t)g_iStopId);
	if (!pMsg) return;
	if (s2sdk::UserMessageGetSerializableMessage(pMsg) == nullptr)
	{
		s2sdk::UserMessageDestroy(pMsg);
		return;
	}
	try
	{
		s2sdk::PbSetInt32(pMsg, "soundevent_guid", iGuid);
		s2sdk::UserMessageSetRecipientMask(pMsg, 1ull << iSlot);
		s2sdk::UserMessageSend(pMsg);
	}
	catch (...)
	{
	}
	s2sdk::UserMessageDestroy(pMsg);
}

} // namespace

bool Init()
{
	// The generated wrappers call these globals; in a process without the s2sdk plugin they are
	// null. GetMessageName/GetMessageID are required for the incoming-message verification.
	g_bAvailable = __s2sdk_UserMessageFindMessageIdByName && __s2sdk_UserMessageCreateFromId
		&& __s2sdk_UserMessageSend && __s2sdk_UserMessageDestroy && __s2sdk_UserMessageGetMessageID
		&& __s2sdk_UserMessageGetMessageName && __s2sdk_UserMessageGetSerializableMessage
		&& __s2sdk_UserMessageGetRecipientMask && __s2sdk_UserMessageSetRecipientMask
		&& __s2sdk_PbReadInt32 && __s2sdk_PbReadUInt32
		&& __s2sdk_PbSetInt32 && __s2sdk_PbSetString && __s2sdk_UserMessageHasField
		&& __s2sdk_HookUserMessage && __s2sdk_UnhookUserMessage;
	g_iStartId = -1;
	g_iSetId = -1;
	g_iStopId = -1;
	return g_bAvailable;
}

void Reset()
{
	g_bAvailable = false;
	g_iStartId = -1;
	g_iSetId = -1;
	g_iStopId = -1;
}

// Resolves the three ids on a live map through the real registry names (the registry matches the
// CMsgSos... names by substring, so the "Sos..." spelling is the stable one; the old GE_ names do
// not match). No hardcoded fallback is ever used: an id that was not resolved means the feature is
// off, because UserMessageCreateFromId(unknown) returns an object whose serializable is null and
// every Pb*/Send/Get* call would dereference it.
bool ResolveIds()
{
	if (!g_bAvailable) return false;
	const int iStart = FindOne("SosStartSoundEvent");
	const int iSet = FindOne("SosSetSoundEventParams");
	const int iStop = FindOne("SosStopSoundEvent");
	if (iStart > 0) g_iStartId = iStart;
	if (iSet > 0) g_iSetId = iSet;
	if (iStop > 0) g_iStopId = iStop;
	return g_iStartId > 0;
}

bool IdsResolved() { return g_bAvailable && g_iStartId > 0; }

int StartMessageId() { return g_bAvailable ? g_iStartId : -1; }
int SetMessageId() { return g_bAvailable ? g_iSetId : -1; }
int StopMessageId() { return g_bAvailable ? g_iStopId : -1; }

bool ParseStart(void* pUserMessage, int& iGuid, uint32_t& uHash)
{
	iGuid = -1;
	uHash = 0;
	if (!g_bAvailable || !pUserMessage || g_iStartId <= 0) return false;

	// Incoming engine-owned message: verify the type before any field access. A wrong id/name
	// (protocol change, a different message hooked by a stale id) never reaches PbRead.
	if (!IsStartMessage(pUserMessage)) return false;
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

void Set(int iSlot, int iGuid, float flVolume) { SendParamsTo(iSlot, iGuid, flVolume); }

void Stop(int iSlot, int iGuid) { SendStopTo(iSlot, iGuid); }

} // namespace sound_volume
