// quake_sounds — per-player volume of a started soundevent (the original's SoundVolume module).
//
// CS2 ignores EmitSound_t::m_flVolume for soundevents, so the original watched
// IGameEventSystem::PostEventAbstract for GE_SosStartSoundEvent and, for every recipient,
// immediately sent him a GE_SosSetSoundEventParams naming the same soundevent_guid with a
// "volume" parameter. This port does the same through s2sdk's user-message API:
//
//   * s2sdk::HookUserMessage(<resolved SosStartSoundEvent id>, cb, HookMode::Post) is the
//     PostEventPost equivalent — s2sdk's user-message hook polyhooks
//     IGameEventSystem::PostEventAbstract with both modes and hands the callback the message,
//     whose recipient filter is intact;
//   * the params message is built with UserMessageCreateFromId(<resolved
//     SosSetSoundEventParams id>) + PbSet* and sent to one recipient mask with UserMessageSend (a
//     low-level CNetMessage::Send, it does not re-enter the PostEventAbstract hook, so no s2sdk
//     lock is taken again).
//
// Message ids are resolved LAZILY on the first live map through the real registry names
// ("SosStartSoundEvent"/"SosSetSoundEventParams"/"SosStopSoundEvent"; the registry matches the
// CMsgSos... names by substring). There is NO hardcoded fallback id: a failed lookup disables the
// corresponding feature, because UserMessageCreateFromId(unknown) returns a non-null object with a
// null serializable and Pb*/Send/GetMessageName on it dereferences null. Start/Stop/Set are checked
// separately; a missing Set/Stop makes only that operation a no-op.

#pragma once

#include <cstdint>

namespace sound_volume {

// Checks the s2sdk bindings; no lookup is done before the first live map. Returns false when the
// bindings are missing; every other function is then a no-op.
bool Init();

// Drops the resolved ids/availability (OnPluginEnd).
void Reset();

// Registry lookup on the first live map. Returns true when SosStartSoundEvent resolved; each id is
// updated independently and no fallback id is used.
bool ResolveIds();
bool IdsResolved();

// Message ids from ResolveIds(); -1 until resolved/unavailable.
int StartMessageId();
int SetMessageId();
int StopMessageId();

// s2sdk::UserMessageCallback payload -> (guid, soundevent hash). False when it is not a sos start
// (the incoming GetMessageName/GetMessageID are checked first), the id changed, or the message
// carries no soundevent_guid (the field is optional in CMsgSosStartSoundEvent).
bool ParseStart(void* pUserMessage, int& iGuid, uint32_t& uHash);

// Volume (1.0 = the soundevent's own) of sound iGuid for one player, and the stop counterpart.
void Set(int iSlot, int iGuid, float flVolume);
void Stop(int iSlot, int iGuid);

// Recipients of the hooked message as a bit mask (0 when the message has none).
uint64_t RecipientMask(void* pUserMessage);

} // namespace sound_volume
