// quake_sounds — engine-free core of the port (config, phrases, kill logic, sound bookkeeping).
//
// The reference is the Metamod port cs2-quake-sounds (itself a port of Kandru's CS2 QuakeSounds).
// This core keeps the *decisions* of the original (which record matches, who hears it, which
// message is printed, which volume applies) and leaves every engine call to src/plugin.cpp, so the
// whole decision layer is testable without a server (test/quake_core_tests.cpp runs it against the
// real quake_sounds.ini).
//
// Naming follows the reference (bEnabled, bIgnoreBots, iDefaultVolume ...) so a diff against the
// original's quake_sounds.cpp stays readable.

#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace qs {

// ---------------------------------------------------------------- strings

std::string Lower(std::string s);
std::string Trim(const std::string& s);
std::string Replace(std::string s, const std::string& from, const std::string& to);
// ";"- or ","-separated list (the config uses both spellings).
std::vector<std::string> SplitList(const std::string& s);

// ---------------------------------------------------------------- entity handles

// HANDLE validity of the raw value (no engine access): a handle is invalid only when it is
// INVALID_EHANDLE_INDEX (-1) or 0. A CEntityHandle is a uint32 view (entity index in the low 15
// bits, serial above); a few minutes after the server starts the serial passes 65535, bit 31 gets
// set and every valid handle reads as a NEGATIVE int (e.g. -921534371 = 0xC912805D = index 93,
// serial 102949). The sign means nothing. 0 is index 0 serial 0 - the world, never a player or a
// weapon - and is also the "none" the handle helpers here return, so it stays invalid.
inline bool IsValidHandle(int32_t h) { return h != -1 && h != 0; }

// ---------------------------------------------------------------- config

// sound_hearable_by / per-entry _filter values of the original (QsScope).
enum class Scope { All, AttackerTeam, VictimTeam, Involved, Attacker, Victim, Spectator };
Scope ParseScope(const std::string& s);

// The three groups of `sound_priorities` (QsKind).
enum class Kind { Special, Weapons, KillStreak };

struct SoundEntry
{
	std::string sKey;
	std::vector<std::string> vSounds;
	std::map<std::string, std::string> mapMessages; // language (lower case) -> text
	float flVolume = -1.0f;                         // -1 = the soundevent's own volume
	bool bHasScope = false;
	Scope eScope = Scope::All;
};

struct Config
{
	bool bEnabled = true;
	bool bDebug = false;
	bool bWarmup = true;
	std::string sPlayOn = "player";
	int iDefaultVolume = 100; // percent (only soundevents honour it)
	Scope eScope = Scope::All;
	bool bIgnoreBots = false;
	bool bIgnoreWorld = true;
	bool bCountSelf = false;
	bool bCountTeam = false;
	bool bResetOnDeath = true;
	bool bResetOnRoundStart = true;
	bool bHeadshotOnce = false;
	std::vector<std::string> vPrecache;
	std::vector<std::string> vCommands = {"!qs"};
	std::vector<std::string> vLangCommands = {"!lang"};
	bool bSettingsMenu = false;
	bool bUtilsSettings = true;
	bool bCenter = true;
	std::string sCenterType = "default"; // default | alert | html
	bool bChat = true;
	std::vector<Kind> vPriority = {Kind::Special, Kind::Weapons, Kind::KillStreak};
};

// Parses the KeyValues text of quake_sounds.ini (the real file, tests feed it verbatim).
// Returns false when the text cannot be parsed; on failure cfg/entries keep their defaults.
bool ParseConfigText(std::string_view text, Config& cfg, std::map<std::string, SoundEntry>& mapEntries);

// ---------------------------------------------------------------- phrases

// The built-in fallbacks of the original (key, ru, en).
struct DefaultPhrase
{
	const char* szKey;
	const char* szRu;
	const char* szEn;
};
const std::vector<DefaultPhrase>& DefaultPhrases();

// Parses quake_sounds.phrases.txt; only the server language (+ "en" fallback) is kept, exactly
// like the original's LoadPhrases().
bool ParsePhrasesText(std::string_view text, const std::string& sServerLang,
					  std::map<std::string, std::string>& mapPhrases);

// Phrase() of the original: store first, then the built-in table, then the key itself; every key
// except "Prefix" gets {PREFIX} expanded.
std::string Phrase(const std::map<std::string, std::string>& mapPhrases, const std::string& sServerLang,
				   const std::string& sKey);

// "ru-RU" -> "ru" (NormalizeLang).
std::string NormalizeLang(const std::string& s);
// Message for one player: his language, then the server language, then the first entry.
std::string ResolveMessage(const SoundEntry& e, const std::string& sPlayerLang, const std::string& sServerLang);

// ---------------------------------------------------------------- sound bookkeeping

// MurmurHash2 (lower-cased input, the s2sdk/engine seed) — how CS2 hashes soundevent names.
uint32_t SoundEventHash(std::string_view name);
// "_volume" record of an entry that is a soundevent (not a sounds/... path); -1 when the file is
// played by the client and no volume applies.
float EntryVolume(const SoundEntry& e);

// The packed GE_SosSetSoundEventParams "volume" parameter (hash + type + size + float), built
// exactly like the original sound_volume.h; exposed so the tests can pin the byte layout.
std::string PackVolumeParam(float flVolume);

// ---------------------------------------------------------------- kill logic

// Whether <iListenerTeam> hears a sound played for that kill (the Matches() switch of the
// original). iSlot/iAttacker/iVictim are slot numbers (-1 = none).
bool MatchesScope(int iSlot, int iListenerTeam, int iAttacker, int iAttackerTeam, int iVictim,
				  int iVictimTeam, Scope eScope);

// first blood: the attacker's only kill so far, and the only entry in the table (the original's
// IsFirstBlood).
bool IsFirstBlood(const std::map<int, int>& mapKills, int iAttacker);

// "listener" mode: the sound is emitted on every listener himself, so the record's volume is
// scaled by that listener's own volume setting.
float ListenerVolume(float flEntry, int iVolumePercent);

// True while the entry's first sound is not a raw file path (i.e. the volume menus make sense).
bool AnyVolumeSound(const std::map<std::string, SoundEntry>& mapEntries);
bool IsFilePathSound(const std::string& sSound);

// Random pick out of a record's sound list (uniform; the callers own the RNG).
std::size_t PickSoundIndex(std::size_t nCount, uint32_t uRandom);
// Weapon classname lookup of the original: "deagle" -> "weapon_deagle", "weapon_x" kept as is.
std::string WeaponSoundKey(const std::string& sWeapon);

// The C4 countdown record of a whole number of remaining seconds ("bomb_3"): the reference built
// the key the same way from ceil(time to explosion).
std::string BombKey(int iSecondsLeft);

} // namespace qs
