// quake_sounds — engine-free core (see quake_core.hpp).
#include "quake_core.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include <pisex/keyvalues.hpp>

namespace qs {

// ---------------------------------------------------------------- strings

std::string Lower(std::string s)
{
	std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
	return s;
}

std::string Trim(const std::string& s)
{
	size_t b = s.find_first_not_of(" \t\r\n");
	if (b == std::string::npos) return "";
	size_t e = s.find_last_not_of(" \t\r\n");
	return s.substr(b, e - b + 1);
}

std::string Replace(std::string s, const std::string& from, const std::string& to)
{
	size_t pos = 0;
	while (!from.empty() && (pos = s.find(from, pos)) != std::string::npos)
	{
		s.replace(pos, from.size(), to);
		pos += to.size();
	}
	return s;
}

std::vector<std::string> SplitList(const std::string& s)
{
	std::vector<std::string> v;
	size_t iStart = 0;
	while (iStart <= s.size())
	{
		size_t iEnd = s.find(',', iStart);
		if (iEnd == std::string::npos) iEnd = s.size();
		std::string sPart = Trim(s.substr(iStart, iEnd - iStart));
		if (!sPart.empty()) v.push_back(sPart);
		iStart = iEnd + 1;
	}
	return v;
}

// ---------------------------------------------------------------- config

Scope ParseScope(const std::string& s)
{
	std::string v = Lower(Trim(s));
	if (v == "attacker_team") return Scope::AttackerTeam;
	if (v == "victim_team") return Scope::VictimTeam;
	if (v == "involved") return Scope::Involved;
	if (v == "attacker") return Scope::Attacker;
	if (v == "victim") return Scope::Victim;
	if (v == "spectator") return Scope::Spectator;
	return Scope::All;
}

bool ParseConfigText(std::string_view text, Config& cfg, std::map<std::string, SoundEntry>& mapEntries)
{
	Config def;
	cfg = def;
	mapEntries.clear();

	pisex::KeyValues kv("QuakeSounds");
	if (!kv.LoadFromBuffer(text)) return false;

	cfg.bEnabled = kv.GetInt("enabled", 1) != 0;
	cfg.bDebug = kv.GetInt("debug", 0) != 0;

	if (pisex::KeyValues* p = kv.FindKey("global"))
	{
		cfg.bWarmup = p->GetInt("enabled_during_warmup", 1) != 0;
		cfg.sPlayOn = Lower(p->GetString("play_on_entity", "player"));
		cfg.iDefaultVolume = std::clamp((int)lroundf(p->GetFloat("default_volume", 1.0f) * 100.0f), 0, 100);
		cfg.eScope = ParseScope(p->GetString("sound_hearable_by", "all"));
		cfg.bIgnoreBots = p->GetInt("ignore_bots", 0) != 0;
		cfg.bIgnoreWorld = p->GetInt("ignore_world_damage", 1) != 0;
		cfg.bCountSelf = p->GetInt("count_self_kills", 0) != 0;
		cfg.bCountTeam = p->GetInt("count_team_kills", 0) != 0;
		cfg.bResetOnDeath = p->GetInt("reset_kills_on_death", 1) != 0;
		cfg.bResetOnRoundStart = p->GetInt("reset_kills_on_round_start", 1) != 0;
		cfg.bHeadshotOnce = p->GetInt("headshot_once", 0) != 0;
	}
	if (pisex::KeyValues* p = kv.FindKey("precache"))
	{
		std::string sFile = p->GetString("soundevent_file", "");
		if (!sFile.empty()) cfg.vPrecache.push_back(sFile);
		if (pisex::KeyValues* pAdd = p->FindKey("additional_files"))
			for (pisex::KeyValues* pKey = pAdd->GetFirstValue(); pKey; pKey = pKey->GetNextValue())
			{
				const char* sz = pKey->GetString(nullptr, "");
				if (sz && sz[0]) cfg.vPrecache.push_back(sz);
			}
	}
	if (pisex::KeyValues* p = kv.FindKey("commands"))
	{
		cfg.vCommands = SplitList(p->GetString("settings", "!qs"));
		cfg.vLangCommands = SplitList(p->GetString("language", "!lang"));
		cfg.bSettingsMenu = p->GetInt("settings_menu", 0) != 0;
		cfg.bUtilsSettings = p->GetInt("utils_settings_menu", 1) != 0;
	}
	if (pisex::KeyValues* p = kv.FindKey("messages"))
	{
		cfg.bCenter = p->GetInt("enable_center_message", 1) != 0;
		cfg.sCenterType = Lower(p->GetString("center_message_type", "default"));
		cfg.bChat = p->GetInt("enable_chat_message", 1) != 0;
	}
	if (pisex::KeyValues* p = kv.FindKey("sound_priorities"))
	{
		std::vector<std::pair<int, Kind>> v = {
			{p->GetInt("special_events", 1), Kind::Special},
			{p->GetInt("weapons", 2), Kind::Weapons},
			{p->GetInt("kill_streak", 3), Kind::KillStreak},
		};
		std::stable_sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
		cfg.vPriority.clear();
		for (auto& x : v) cfg.vPriority.push_back(x.second);
	}
	if (pisex::KeyValues* pSounds = kv.FindKey("sounds"))
	{
		for (pisex::KeyValues* pEntry = pSounds->GetFirstTrueSubKey(); pEntry; pEntry = pEntry->GetNextTrueSubKey())
		{
			SoundEntry e;
			e.sKey = Lower(pEntry->GetName());
			for (pisex::KeyValues* pVal = pEntry->GetFirstValue(); pVal; pVal = pVal->GetNextValue())
			{
				std::string sName = Lower(pVal->GetName());
				std::string sValue = pVal->GetString(nullptr, "");
				if (sName == "_sound") e.vSounds = SplitList(sValue);
				else if (sName == "_filter") { e.bHasScope = true; e.eScope = ParseScope(sValue); }
				else if (sName == "_volume") e.flVolume = (float)atof(sValue.c_str());
				else if (!sName.empty() && sName[0] != '_' && !sValue.empty()) e.mapMessages[sName] = sValue;
			}
			if (e.vSounds.empty()) continue; // like the original: a record without _sound never plays
			mapEntries[e.sKey] = e;
		}
	}
	return true;
}

// ---------------------------------------------------------------- phrases

const std::vector<DefaultPhrase>& DefaultPhrases()
{
	static const std::vector<DefaultPhrase> v = {
		{"Prefix", "[QuakeSounds]", "[QuakeSounds]"},
		{"Muted", "{PREFIX} {DEFAULT}Quake-звуки для вас выключены.", "{PREFIX} {DEFAULT}Muted all sounds for yourself."},
		{"Unmuted", "{PREFIX} {DEFAULT}Quake-звуки для вас включены.", "{PREFIX} {DEFAULT}Unmuted all sounds for yourself."},
		{"Menu_Title", "Quake-звуки", "Quake Sounds"},
		{"Menu_Mute", "Выключить звуки", "Mute Sounds"},
		{"Menu_Unmute", "Включить звуки", "Unmute Sounds"},
		{"Menu_Volume", "Громкость: {volume}%", "Volume: {volume}%"},
		{"Volume_Title", "Громкость: {volume}%", "Volume: {volume}%"},
		{"Volume_Up", "Громче (+10%)", "Louder (+10%)"},
		{"Volume_Down", "Тише (−10%)", "Quieter (−10%)"},
		{"Settings_Item", "Quake-звуки", "Quake sounds"},
		{"Center_Msg_Player", "{message}", "{message}"},
		{"Center_Msg_Other", "{player} -> {message}", "{player} -> {message}"},
		{"Chat_Msg_Player", "{PREFIX} {DEFAULT}Достижение: {GREEN}{message}", "{PREFIX} {DEFAULT}Achievement: {GREEN}{message}"},
		{"Chat_Msg_Other", "{PREFIX} {GREEN}{player} {DEFAULT}-> {GREEN}{message}", "{PREFIX} {GREEN}{player} {DEFAULT}-> {GREEN}{message}"},
		{"Lang_Set", "{PREFIX} {DEFAULT}Язык сообщений: {GREEN}{lang}", "{PREFIX} {DEFAULT}Message language: {GREEN}{lang}"},
		{"Lang_Usage", "{PREFIX} {DEFAULT}Использование: !lang <код языка>, например !lang ru", "{PREFIX} {DEFAULT}Usage: !lang <language code>, e.g. !lang en"},
	};
	return v;
}

bool ParsePhrasesText(std::string_view text, const std::string& sServerLang,
					  std::map<std::string, std::string>& mapPhrases)
{
	mapPhrases.clear();
	pisex::KeyValues kv("Phrases");
	if (!kv.LoadFromBuffer(text)) return false;
	for (pisex::KeyValues* pKey = kv.GetFirstTrueSubKey(); pKey; pKey = pKey->GetNextTrueSubKey())
	{
		const char* sz = pKey->GetString(sServerLang.c_str(), "");
		if (!sz[0]) sz = pKey->GetString("en", "");
		if (sz[0]) mapPhrases[pKey->GetName()] = sz;
	}
	return true;
}

std::string Phrase(const std::map<std::string, std::string>& mapPhrases, const std::string& sServerLang,
				   const std::string& sKey)
{
	std::string s;
	auto it = mapPhrases.find(sKey);
	if (it != mapPhrases.end()) s = it->second;
	else
	{
		s = sKey;
		for (const DefaultPhrase& d : DefaultPhrases())
			if (sKey == d.szKey) { s = sServerLang == "ru" ? d.szRu : d.szEn; break; }
	}
	if (sKey != "Prefix") s = Replace(s, "{PREFIX}", Phrase(mapPhrases, sServerLang, "Prefix"));
	return s;
}

std::string NormalizeLang(const std::string& s)
{
	std::string v = Lower(Trim(s));
	size_t iDash = v.find('-');
	if (iDash != std::string::npos) v = v.substr(0, iDash);
	return v;
}

std::string ResolveMessage(const SoundEntry& e, const std::string& sPlayerLang, const std::string& sServerLang)
{
	if (e.mapMessages.empty()) return "";
	if (!sPlayerLang.empty())
	{
		auto it = e.mapMessages.find(NormalizeLang(sPlayerLang));
		if (it != e.mapMessages.end()) return it->second;
	}
	auto it = e.mapMessages.find(NormalizeLang(sServerLang));
	if (it != e.mapMessages.end()) return it->second;
	return e.mapMessages.begin()->second;
}

// ---------------------------------------------------------------- sound bookkeeping

uint32_t SoundEventHash(std::string_view name)
{
	std::string s = Lower(std::string(name));
	const uint32_t m = 0x5bd1e995;
	uint32_t len = (uint32_t)s.size();
	uint32_t h = 0x53524332u ^ len;
	const unsigned char* p = (const unsigned char*)s.data();
	while (len >= 4)
	{
		uint32_t k;
		memcpy(&k, p, 4);
		k *= m; k ^= k >> 24; k *= m;
		h *= m; h ^= k;
		p += 4; len -= 4;
	}
	switch (len)
	{
		case 3: h ^= (uint32_t)p[2] << 16; [[fallthrough]];
		case 2: h ^= (uint32_t)p[1] << 8; [[fallthrough]];
		case 1: h ^= p[0]; h *= m;
		default: break;
	}
	h ^= h >> 13; h *= m; h ^= h >> 15;
	return h;
}

bool IsFilePathSound(const std::string& sSound)
{
	return sSound.size() >= 7 && strncasecmp(sSound.c_str(), "sounds/", 7) == 0;
}

float EntryVolume(const SoundEntry& e)
{
	return e.flVolume >= 0.0f ? e.flVolume : 1.0f;
}

std::string PackVolumeParam(float flVolume)
{
	// hash("volume", 0xB5D9F700) | type 4 (float) | size 4 | 0 | value
	uint32_t uVolumeHash = 0;
	{
		// MurmurHash2 with the parameter seed, same helper as the event names.
		std::string s = "volume";
		const uint32_t m = 0x5bd1e995;
		uint32_t len = (uint32_t)s.size();
		uint32_t h = 0xB5D9F700u ^ len;
		const unsigned char* p = (const unsigned char*)s.data();
		while (len >= 4)
		{
			uint32_t k;
			memcpy(&k, p, 4);
			k *= m; k ^= k >> 24; k *= m;
			h *= m; h ^= k;
			p += 4; len -= 4;
		}
		// The MurmurHash2 tail ("volume" leaves 2 bytes): without it the hash is not the one the
		// client maps to the volume parameter, and the per-player volume was never applied.
		switch (len)
		{
			case 3: h ^= (uint32_t)p[2] << 16; [[fallthrough]];
			case 2: h ^= (uint32_t)p[1] << 8; [[fallthrough]];
			case 1: h ^= p[0]; h *= m; break;
			default: break;
		}
		h ^= h >> 13; h *= m; h ^= h >> 15;
		uVolumeHash = h;
	}
	std::string sPacked;
	sPacked.append((const char*)&uVolumeHash, 4);
	sPacked.push_back((char)4);
	sPacked.push_back((char)4);
	sPacked.push_back((char)0);
	sPacked.append((const char*)&flVolume, 4);
	return sPacked;
}

// ---------------------------------------------------------------- kill logic

bool MatchesScope(int iSlot, int iListenerTeam, int iAttacker, int iAttackerTeam, int iVictim,
				  int iVictimTeam, Scope eScope)
{
	switch (eScope)
	{
	case Scope::AttackerTeam: return iAttacker >= 0 && iListenerTeam == iAttackerTeam;
	case Scope::VictimTeam: return iVictim >= 0 && iListenerTeam == iVictimTeam;
	case Scope::Involved: return iSlot == iAttacker || iSlot == iVictim;
	case Scope::Attacker: return iSlot == iAttacker;
	case Scope::Victim: return iSlot == iVictim;
	case Scope::Spectator: return iListenerTeam == 1;
	default: return true;
	}
}

bool IsFirstBlood(const std::map<int, int>& mapKills, int iAttacker)
{
	auto it = mapKills.find(iAttacker);
	return it != mapKills.end() && mapKills.size() == 1 && it->second == 1;
}

float ListenerVolume(float flEntry, int iVolumePercent)
{
	return flEntry * (float)iVolumePercent / 100.0f;
}

bool AnyVolumeSound(const std::map<std::string, SoundEntry>& mapEntries)
{
	for (const auto& [sKey, e] : mapEntries)
		for (const auto& s : e.vSounds)
			if (!IsFilePathSound(s)) return true;
	return false;
}

std::size_t PickSoundIndex(std::size_t nCount, uint32_t uRandom)
{
	if (nCount == 0) return 0;
	return (std::size_t)(uRandom % (uint32_t)nCount);
}

std::string WeaponSoundKey(const std::string& sWeapon)
{
	if (sWeapon.size() >= 7 && sWeapon.compare(0, 7, "weapon_") == 0) return sWeapon;
	return "weapon_" + sWeapon;
}

std::string BombKey(int iSecondsLeft)
{
	return "bomb_" + std::to_string(iSecondsLeft);
}

} // namespace qs
