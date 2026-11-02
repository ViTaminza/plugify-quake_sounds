// quake_sounds — config/phrase file resolution (see quake_config.hpp).
#include "quake_config.hpp"

#include <fstream>
#include <sstream>
#include <utility>

#include "kv1.hpp"

namespace qs {
namespace {

constexpr char kRawConfigPath[] = "addons/configs/quake_sounds.ini";
constexpr char kRawPhrasesPath[] = "addons/translations/quake_sounds.phrases.txt";
constexpr char kConfigFile[] = "quake_sounds.ini";
constexpr char kPhrasesFile[] = "quake_sounds.phrases.txt";
constexpr char kPluginDir[] = "quake_sounds";

std::string PluginDir(std::string_view root)
{
	return root.empty() ? std::string(kPluginDir) : JoinPath(root, kPluginDir);
}

std::string TriedList(const std::vector<std::string>& candidates)
{
	std::string tried;
	for (const std::string& path : candidates)
	{
		if (!tried.empty()) tried += " or ";
		tried += path;
	}
	return tried;
}

} // namespace

std::string JoinPath(std::string_view dir, std::string_view name)
{
	if (dir.empty()) return std::string(name);
	if (name.empty()) return std::string(dir);
	std::string path(dir);
	if (path.back() != '/') path.push_back('/');
	if (name.front() == '/') name.remove_prefix(1);
	path.append(name);
	return path;
}

bool FileExists(const std::string& path)
{
	if (path.empty()) return false;
	std::ifstream file(path, std::ios::binary);
	return file.good();
}

std::optional<std::string> ReadTextFile(const std::string& path)
{
	if (path.empty()) return std::nullopt;
	std::ifstream file(path, std::ios::binary);
	if (!file) return std::nullopt;
	std::ostringstream buffer;
	buffer << file.rdbuf();
	if (!file.good() && !file.eof()) return std::nullopt;
	return buffer.str();
}

std::vector<std::string> ConfigCandidates(std::string_view configsDir, std::string_view dataDir)
{
	const std::string pluginDir = PluginDir(configsDir);
	const std::string dataPluginDir = PluginDir(dataDir);
	std::vector<std::string> candidates;
	candidates.reserve(4);
	// HEAD's FilePath() was pisex::ConfigDir()+"/"+name: the direct <configs>/quake_sounds/ file
	// must stay first. The raw Metamod spelling and the data/translations copies are only fallbacks
	// for hybrid installations where the direct Plugify file does not exist yet.
	candidates.push_back(JoinPath(pluginDir, kConfigFile));
	candidates.emplace_back(kRawConfigPath);
	candidates.push_back(JoinPath(JoinPath(pluginDir, "translations"), kConfigFile));
	candidates.push_back(JoinPath(dataPluginDir, kConfigFile));
	return candidates;
}

std::vector<std::string> PhraseCandidates(std::string_view configsDir)
{
	const std::string pluginDir = PluginDir(configsDir);
	const std::string translationsDir = JoinPath(pluginDir, "translations");
	const std::string fullPath(kRawPhrasesPath);
	std::vector<std::string> candidates;
	candidates.reserve(4);
	// HEAD's phrases path was pisex::TranslationsDir()+"/quake_sounds.phrases.txt": the direct
	// plugin translations file first, then the raw Metamod spelling and the old pisexcompat
	// full-path probes as fallbacks.
	candidates.push_back(JoinPath(translationsDir, kPhrasesFile));
	candidates.emplace_back(kRawPhrasesPath);
	candidates.push_back(JoinPath(translationsDir, fullPath));
	candidates.push_back(JoinPath(pluginDir, fullPath));
	return candidates;
}

ConfigLoad LoadConfigFromDisk(std::string_view configsDir, std::string_view dataDir, Config& cfg,
							  std::map<std::string, SoundEntry>& entries)
{
	ConfigLoad result;
	const std::vector<std::string> candidates = ConfigCandidates(configsDir, dataDir);
	result.path = candidates.empty() ? std::string(kConfigFile) : candidates.front();
	for (const std::string& path : candidates)
	{
		if (!FileExists(path)) continue;
		result.path = path;
		const std::optional<std::string> text = ReadTextFile(path);
		if (!text)
		{
			result.error = "failed to read " + path;
			return result;
		}
		if (!ParseConfigText(*text, cfg, entries))
		{
			// The first existing file is authoritative: a malformed primary config is reported,
			// never masked by the plugin-directory copy.
			result.error = "failed to load " + path + " (not valid KeyValues)";
			return result;
		}
		result.loaded = true;
		return result;
	}
	result.error = "failed to load " + TriedList(candidates);
	return result;
}

PhrasesLoad LoadPhrasesFromDisk(std::string_view configsDir, const std::string& language,
								std::map<std::string, std::string>& phrases)
{
	PhrasesLoad result;
	const std::vector<std::string> candidates = PhraseCandidates(configsDir);
	result.path = candidates.empty() ? std::string(kPhrasesFile) : candidates.front();
	for (const std::string& path : candidates)
	{
		if (!FileExists(path)) continue;
		result.path = path;
		const std::optional<std::string> text = ReadTextFile(path);
		if (!text)
		{
			result.error = "failed to read " + path;
			return result;
		}
		if (!ParsePhrasesText(*text, language, phrases))
		{
			result.error = "failed to load " + path + " (not valid KeyValues)";
			return result;
		}
		result.loaded = true;
		return result;
	}
	result.error = "failed to load " + TriedList(candidates);
	return result;
}

std::string ReadCoreConfigLanguage(std::string_view configsDir)
{
	if (configsDir.empty()) return {};
	const std::string path = JoinPath(configsDir, "core.cfg");
	const std::optional<std::string> text = ReadTextFile(path);
	if (!text) return {};
	const std::optional<KvNode> root = ParseKv1(*text);
	if (!root) return {};
	return std::string(root->GetString("ServerLang", ""));
}

} // namespace qs
