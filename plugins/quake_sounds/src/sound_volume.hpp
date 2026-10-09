// quake_sounds — per-player volume of a started soundevent (the SoundVolume namespace of the
// reference: ref/src/cs2-quake-sounds/include/sound_volume.h).
//
// CS2 ignores EmitSound_t::m_flVolume for soundevents, so the original watched
// IGameEventSystem::PostEventAbstract for GE_SosStartSoundEvent (208) and, for every recipient,
// immediately sent him a GE_SosSetSoundEventParams (210) naming the same soundevent_guid with a
// "volume" parameter. This port does the same through s2sdk's user-message API:
//
//   * s2sdk::HookUserMessage(208, cb, HookMode::Post) is the PostEventPost equivalent — s2sdk
//     polyhooks IGameEventSystem::PostEventAbstract with both modes (see ref/s2sdk-src
//     src/core/plugin.cpp) and hands the callback the message, whose recipient filter is intact;
//   * the params message is built with UserMessageCreateFromId(210) + PbSet* and sent to one
//     recipient mask with UserMessageSend (a low-level CNetMessage::Send, it does not re-enter
//     the PostEventAbstract hook, so no s2sdk lock is taken again).
//
// Message ids are resolved by name at first use ("GE_SosStartSoundEvent"/"GE_SosSetSoundEventParams")
// and fall back to the well-known 208/210 constants (EBaseGameEvents) when the lookup fails.

#pragma once

#include <cstdint>

namespace sound_volume {

// Resolves the message ids and the "volume" parameter hash. Returns false when the s2sdk
// bindings are missing; every other function is then a no-op.
bool Init();

// Message id to pass to s2sdk::HookUserMessage (HookMode::Post); -1 when unavailable.
int StartMessageId();
// Message id of GE_SosSetSoundEventParams; -1 when unavailable.
int SetMessageId();
// GE_SosStopSoundEvent (the fade-out of round_end_sounds uses it).
int StopMessageId();

// s2sdk::UserMessageCallback payload -> (guid, soundevent hash). False when it is not a sos start
// or the message carries no soundevent_guid (the field is optional in CMsgSosStartSoundEvent).
bool ParseStart(void* pUserMessage, int& iGuid, uint32_t& uHash);

// Volume (1.0 = the soundevent's own) of sound iGuid for one player, and the stop counterpart.
void Set(int iSlot, int iGuid, float flVolume);
void Stop(int iSlot, int iGuid);

// Recipients of the hooked message as a bit mask (0 when the message has none).
uint64_t RecipientMask(void* pUserMessage);

} // namespace sound_volume
