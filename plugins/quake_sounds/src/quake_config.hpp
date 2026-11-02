// quake_sounds — config/phrase file resolution of the port (engine-free, testable).
//
// The files are the production Metamod files, read unchanged by the plugin-local KV1 reader:
//
//   configs/quake_sounds/quake_sounds.ini
//   configs/quake_sounds/translations/quake_sounds.phrases.txt
//
// The candidate order restores HEAD's direct plugin-dir paths first; the raw/addons and data
// spellings are only fallbacks for hybrid installations:
//
//   1. <configs>/quake_sounds/quake_sounds.ini         (HEAD FilePath: ConfigDir() + file)
//   2. addons/configs/quake_sounds.ini                 (raw Metamod path, cwd)
//   3. <configs>/quake_sounds/translations/quake_sounds.ini
//   4. <data>/quake_sounds/quake_sounds.ini            (DataDir())
//
// That keeps a stale raw file from silently shadowing a maintained Plugify config. The first file
// that exists is authoritative: a malformed primary file is reported, never masked by a later
// candidate (the original's LoadFromFile failure left the defaults in place).
//
// Phrases: HEAD read pisex::TranslationsDir()+"/"+"quake_sounds.phrases.txt", so the direct
// <configs>/quake_sounds/translations/quake_sounds.phrases.txt comes first, then the raw cwd file
// and the old pisexcompat full-path probes.

#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "quake_core.hpp"

namespace qs {

std::string JoinPath(std::string_view dir, std::string_view name);
bool FileExists(const std::string& path);
std::optional<std::string> ReadTextFile(const std::string& path);

std::vector<std::string> ConfigCandidates(std::string_view configsDir, std::string_view dataDir);
std::vector<std::string> PhraseCandidates(std::string_view configsDir);

struct ConfigLoad
{
	bool loaded = false;
	std::string path;  // the file that was read (or the first candidate when nothing existed)
	std::string error; // human-readable reason, empty when loaded
};

struct PhrasesLoad
{
	bool loaded = false;
	std::string path;
	std::string error;
};

// Reads the first existing candidate and parses it. On failure cfg/entries are left at their
// defaults (ParseConfigText resets them) and `error` names the file/paths that were tried.
ConfigLoad LoadConfigFromDisk(std::string_view configsDir, std::string_view dataDir, Config& cfg,
							  std::map<std::string, SoundEntry>& entries);

// Same for the phrase table. `language` is the server language already normalised by
// PickLanguage(); only that language (+ "en") is kept, exactly like the original's LoadPhrases().
PhrasesLoad LoadPhrasesFromDisk(std::string_view configsDir, const std::string& language,
								std::map<std::string, std::string>& phrases);

// <configs>/core.cfg "ServerLang" (the Pisex/cs2-menus server language); "" when the file is
// missing, malformed or has no key.
std::string ReadCoreConfigLanguage(std::string_view configsDir);

} // namespace qs
