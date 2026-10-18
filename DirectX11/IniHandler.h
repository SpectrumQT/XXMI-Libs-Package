#include "HackerDevice.h"

void FlagConfigReload(HackerDevice *device, void *private_data);
void ToggleInput(HackerDevice *device, void *private_data);
void LoadConfigFile();
void ReloadConfig(HackerDevice *device);
void LoadSkippedIncludes();
// Whether [ShaderOverride] sections with a hash_dx12 were read without
// knowing whether the game is D3D12, and so went by their D3D11 hash
bool IniDx12HashesProvisional();
void RegisterUnknownSetting(const wchar_t* name, float value);
bool SavePersistentSettings(bool force = false);
bool SaveUnknownPersistentSettings();
void SavePersistentSettingsInBackground();
void CleanUpPersistentSettingsSaves();

struct IniLine {
	// Same syntax as std::pair, whitespace stripped around each:
	wstring first;
	wstring second;

	// For when we don't want whitespace around the equals sign stripped,
	// or when there is no equals sign (whitespace at the start and end of
	// the whole line is still stripped):
	wstring raw_line;

	// Namespaced sections can determine the namespace from the section as
	// a whole, but global sections like [Present] can have lines from many
	// different namespaces, so each line stores the namespace it came from
	// to resolve references within the namespace:
	wstring ini_namespace;

	// Only set when the ini file overrode its namespace, so the path of the
	// file this line came from can still be recovered (mirrors
	// IniSection::ini_path for lines of global sections such as [Constants]).
	// Points into a registry owned by the ini parser that lives as long as
	// the parsed sections, so this costs a pointer rather than a string per
	// line:
	const wstring *ini_path;

	IniLine(wstring &key, wstring &val, wstring &line, const wstring &ini_namespace, const wstring *ini_path) :
		first(key),
		second(val),
		raw_line(line),
		ini_namespace(ini_namespace),
		ini_path(ini_path)
	{}
};

// Whereas settings within a section are in the same order they were in the ini
// file. This will become more important as shader overrides gains more
// functionality and dependencies between different features form:
typedef std::vector<IniLine> IniSectionVector;

template<typename T>
struct IniValueTypeName
{
	static constexpr const wchar_t* value = L"value";
};

template<>
struct IniValueTypeName<float>
{
	static constexpr const wchar_t* value = L"floating-point";
};

template<>
struct IniValueTypeName<int>
{
	static constexpr const wchar_t* value = L"integer";
};

template<>
struct IniValueTypeName<bool>
{
	static constexpr const wchar_t* value = L"boolean";
};

void GetIniSection(IniSectionVector **key_vals, const wchar_t *section);
int GetIniInt(const wchar_t *section, const wchar_t *key, int def, bool *found, bool warn=true);
bool GetIniBool(const wchar_t *section, const wchar_t *key, bool def, bool *found, bool warn=true);
float GetIniFloat(const wchar_t *section, const wchar_t *key, float def, bool *found);
int GetIniString(const wchar_t *section, const wchar_t *key, const wchar_t *def,
		 wchar_t *ret, unsigned size);
bool GetIniString(const wchar_t *section, const wchar_t *key, const wchar_t *def, std::string *ret);
int GetIniStringAndLog(const wchar_t *section, const wchar_t *key, const wchar_t *def,
		 wchar_t *ret, unsigned size);
template <class T1, class T2>
T2 GetIniEnumClass(const wchar_t *section, const wchar_t *key, T2 def, bool *found,
		struct EnumName_t<T1, T2> *enum_names);

bool ParseBinaryLiterals(const wstring& input, size_t start, uint64_t& out, size_t& length);

inline wchar_t ascii_tolower(wchar_t c);
bool get_namespaced_section_name_lower(const wstring *section, const wstring *ini_namespace, wstring *ret);
bool get_section_namespace(const wchar_t *section, wstring *ret);
wstring get_namespaced_var_name_lower(const wstring& low_name, const wstring* ini_namespace);

// A [ShaderRegex...] group of the loaded config as it is written, for
// vk/SpirvConvert.cpp to make SPIR-V versions of D3D11 patches with
struct ShaderRegexSource {
	// The group's section with its namespace, split before the group's name:
	// "ShaderRegex\<namespace>\" or "ShaderRegex", and the name after that
	wstring prefix, name;
	// The ini file it is in
	wstring file;
	// The lines of its sections, by the lower case name after the group's
	// ("" for its main section, "pattern.replace", "insertdeclarations", ...)
	std::map<wstring, std::vector<std::string>> sections;
};
// The groups of the config, and the ini files with a section whose name has
// "vulkan" in it. Call on the thread that reloads the config.
void GetShaderRegexSources(std::vector<ShaderRegexSource> *groups, std::set<wstring> *files_with_vulkan_sections);

CommandListVariable* RegisterGlobalVariable(wstring& name, float* fval, VariableFlags flags);
const wstring& persistent_variable_save_name(const CommandListVariable* var);
