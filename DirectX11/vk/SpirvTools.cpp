#include "SpirvTools.h"

#include <windows.h>

#include "../../SPIRV-Tools/include/spirv-tools/libspirv.h"
#include "VkDump.h"
#include "../globals.h"
#include "log.h"

#define SPIRV_FUNCS(X) \
	X(spvContextCreate) \
	X(spvContextDestroy) \
	X(spvBinaryToText) \
	X(spvTextDestroy) \
	X(spvTextToBinaryWithOptions) \
	X(spvBinaryDestroy) \
	X(spvValidateWithOptions) \
	X(spvValidatorOptionsCreate) \
	X(spvValidatorOptionsDestroy) \
	X(spvValidatorOptionsSetRelaxBlockLayout) \
	X(spvValidatorOptionsSetScalarBlockLayout) \
	X(spvValidatorOptionsSetSkipBlockLayout) \
	X(spvDiagnosticDestroy)

static struct {
#define X(name) decltype(&::name) name;
	SPIRV_FUNCS(X)
#undef X
} api;

static SRWLOCK lock = SRWLOCK_INIT;
static bool tried, loaded;
// By spv_target_env, made when first needed and kept
static spv_context contexts[SPV_ENV_MAX];
static spv_validator_options validator_options[SPIRV_LAYOUT_COUNT];

static bool load()
{
	wchar_t path[MAX_PATH];
	wchar_t *slash;
	HMODULE dll;
	int i;

	if (tried)
		return loaded;
	tried = true;

	if (!GetModuleFileName(migoto_handle, path, MAX_PATH) || !(slash = wcsrchr(path, L'\\')))
		return false;
	wcscpy_s(slash + 1, MAX_PATH - (slash + 1 - path), L"SPIRV-Tools-shared.dll");
	dll = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
	if (!dll) {
		LogInfo("%S is not there: ShaderRegex sections with SpirvPattern do nothing\n", path);
		VkDumpNote("*** %S is not there: ShaderRegex sections with SpirvPattern do nothing\n", path);
		return false;
	}
#define X(name) api.name = (decltype(api.name))GetProcAddress(dll, #name); if (!api.name) goto missing;
	SPIRV_FUNCS(X)
#undef X

	for (i = 0; i < SPIRV_LAYOUT_COUNT; i++) {
		validator_options[i] = api.spvValidatorOptionsCreate();
		if (!validator_options[i])
			goto missing;
	}
	api.spvValidatorOptionsSetRelaxBlockLayout(validator_options[SPIRV_LAYOUT_RELAXED], true);
	api.spvValidatorOptionsSetScalarBlockLayout(validator_options[SPIRV_LAYOUT_SCALAR], true);
	api.spvValidatorOptionsSetSkipBlockLayout(validator_options[SPIRV_LAYOUT_SKIP], true);

	LogInfo("Loaded %S\n", path);
	VkDumpNote("Loaded %S\n", path);
	loaded = true;
	return true;

missing:
	LogInfo("%S is not one this DLL can use: ShaderRegex sections with SpirvPattern do nothing\n", path);
	VkDumpNote("*** %S is not one this DLL can use: ShaderRegex sections with SpirvPattern do nothing\n", path);
	return false;
}

bool SpirvToolsAvailable()
{
	bool available;

	AcquireSRWLockExclusive(&lock);
	available = load();
	ReleaseSRWLockExclusive(&lock);
	return available;
}

static spv_context context(spv_target_env env)
{
	spv_context made;

	AcquireSRWLockExclusive(&lock);
	if (!load()) {
		made = NULL;
	} else {
		if (!contexts[env])
			contexts[env] = api.spvContextCreate(env);
		made = contexts[env];
	}
	ReleaseSRWLockExclusive(&lock);
	return made;
}

static spv_target_env universal_env(uint32_t version)
{
	switch (version >> 8 & 0xffff) {
		case 0x0100: return SPV_ENV_UNIVERSAL_1_0;
		case 0x0101: return SPV_ENV_UNIVERSAL_1_1;
		case 0x0102: return SPV_ENV_UNIVERSAL_1_2;
		case 0x0103: return SPV_ENV_UNIVERSAL_1_3;
		case 0x0104: return SPV_ENV_UNIVERSAL_1_4;
		case 0x0105: return SPV_ENV_UNIVERSAL_1_5;
	}
	return SPV_ENV_UNIVERSAL_1_6;
}

// The oldest Vulkan that takes that version of SPIR-V
static spv_target_env vulkan_env(uint32_t version)
{
	if (version <= 0x00010000)
		return SPV_ENV_VULKAN_1_0;
	if (version <= 0x00010300)
		return SPV_ENV_VULKAN_1_1;
	if (version <= 0x00010500)
		return SPV_ENV_VULKAN_1_2;
	return SPV_ENV_VULKAN_1_3;
}

static void take_diagnostic(spv_diagnostic diagnostic, const char *fallback, std::string *error)
{
	if (error) {
		if (diagnostic && diagnostic->error) {
			char where[64];

			*error = diagnostic->error;
			if (diagnostic->isTextSource) {
				_snprintf_s(where, _TRUNCATE, " (line %Iu, column %Iu)", diagnostic->position.line + 1, diagnostic->position.column + 1);
				*error += where;
			}
		} else {
			*error = fallback;
		}
	}
	if (diagnostic)
		api.spvDiagnosticDestroy(diagnostic);
}

bool SpirvDisassemble(const uint32_t *code, size_t words, bool friendly, std::string *text, std::string *error)
{
	spv_context made;
	spv_diagnostic diagnostic = NULL;
	spv_text result = NULL;

	if (words < 5 || !(made = context(universal_env(code[1])))) {
		if (error)
			*error = words < 5 ? "not SPIR-V" : "SPIRV-Tools is not loaded";
		return false;
	}
	if (api.spvBinaryToText(made, code, words, SPV_BINARY_TO_TEXT_OPTION_NO_HEADER
			| (friendly ? SPV_BINARY_TO_TEXT_OPTION_FRIENDLY_NAMES : 0), &result, &diagnostic) != SPV_SUCCESS || !result) {
		take_diagnostic(diagnostic, "could not be disassembled", error);
		api.spvTextDestroy(result);
		return false;
	}
	text->assign(result->str, result->length);
	api.spvTextDestroy(result);
	take_diagnostic(diagnostic, "", NULL);
	return true;
}

bool SpirvAssemble(const std::string &text, uint32_t version, bool preserve_ids, std::vector<uint32_t> *code, std::string *error)
{
	spv_context made = context(universal_env(version));
	spv_diagnostic diagnostic = NULL;
	spv_binary result = NULL;

	if (!made) {
		if (error)
			*error = "SPIRV-Tools is not loaded";
		return false;
	}
	if (api.spvTextToBinaryWithOptions(made, text.c_str(), text.size(),
			preserve_ids ? SPV_TEXT_TO_BINARY_OPTION_PRESERVE_NUMERIC_IDS : SPV_TEXT_TO_BINARY_OPTION_NONE,
			&result, &diagnostic) != SPV_SUCCESS || !result || result->wordCount < 5) {
		take_diagnostic(diagnostic, "could not be assembled", error);
		api.spvBinaryDestroy(result);
		return false;
	}
	code->assign(result->code, result->code + result->wordCount);
	(*code)[1] = version;
	api.spvBinaryDestroy(result);
	take_diagnostic(diagnostic, "", NULL);
	return true;
}

bool SpirvValidate(const uint32_t *code, size_t words, SpirvLayoutRules rules, std::string *error)
{
	spv_context made;
	spv_diagnostic diagnostic = NULL;
	spv_const_binary_t binary = { code, words };

	if (words < 5 || !(made = context(vulkan_env(code[1])))) {
		if (error)
			*error = words < 5 ? "not SPIR-V" : "SPIRV-Tools is not loaded";
		return false;
	}
	if (api.spvValidateWithOptions(made, validator_options[rules], &binary, &diagnostic) != SPV_SUCCESS) {
		take_diagnostic(diagnostic, "is not valid", error);
		return false;
	}
	take_diagnostic(diagnostic, "", NULL);
	return true;
}
