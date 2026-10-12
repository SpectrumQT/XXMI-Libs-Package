#include "Dxc.h"

#include <d3dcompiler.h>
#include <dxcapi.h>

#include <map>
#include <unordered_map>

#include "Dx12Common.h"
#include "HashDump.h"
#include "PipelineRegistry.h"
#include "util.h"
#include "../globals.h"

static SRWLOCK lock = SRWLOCK_INIT;
static bool tried;
static IDxcCompiler3 *compiler;
static IDxcUtils *utils;

static bool load()
{
	wchar_t path[MAX_PATH];
	size_t dir;
	HMODULE dll;
	DxcCreateInstanceProc create;

	if (tried)
		return compiler != NULL;
	tried = true;

	if (!GetModuleFileName(migoto_handle, path, MAX_PATH))
		return false;
	dir = wcsrchr(path, L'\\') - path + 1;

	// The validator is looked for by name when a shader is compiled, which
	// finds it once it is loaded. Without it the DXIL is not signed and
	// D3D12 does not take it.
	wcscpy_s(path + dir, MAX_PATH - dir, L"dxil.dll");
	if (!LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH))
		Dx12DumpNote("*** %S is not there: the shaders of mods cannot be compiled to DXIL\n", path);
	wcscpy_s(path + dir, MAX_PATH - dir, L"dxcompiler.dll");
	dll = LoadLibraryExW(path, NULL, LOAD_WITH_ALTERED_SEARCH_PATH);
	if (!dll) {
		Dx12DumpNote("*** %S is not there: the shaders of mods cannot be compiled to DXIL\n", path);
		return false;
	}
	create = (DxcCreateInstanceProc)GetProcAddress(dll, "DxcCreateInstance");
	if (!create || FAILED(create(CLSID_DxcCompiler, IID_PPV_ARGS(&compiler))) || FAILED(create(CLSID_DxcUtils, IID_PPV_ARGS(&utils)))) {
		Dx12DumpNote("*** %S could not be set up: the shaders of mods cannot be compiled to DXIL\n", path);
		if (compiler)
			compiler->Release();
		if (utils)
			utils->Release();
		compiler = NULL;
		utils = NULL;
		return false;
	}
	Dx12DumpNote("Loaded %S\n", path);
	return true;
}

bool Dx12DxcAvailable()
{
	bool available;

	AcquireSRWLockExclusive(&lock);
	available = load();
	ReleaseSRWLockExclusive(&lock);
	return available;
}

static std::wstring widen(const std::string &text)
{
	std::wstring wide(MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), NULL, 0), 0);

	MultiByteToWideChar(CP_UTF8, 0, text.data(), (int)text.size(), &wide[0], (int)wide.size());
	return wide;
}

static std::string narrow(const std::wstring &text)
{
	std::string utf8(WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), NULL, 0, NULL, NULL), 0);

	WideCharToMultiByte(CP_UTF8, 0, text.data(), (int)text.size(), &utf8[0], (int)utf8.size(), NULL, NULL);
	return utf8;
}

static std::wstring full_path(const std::wstring &path)
{
	wchar_t full[MAX_PATH];
	DWORD length = GetFullPathNameW(path.c_str(), MAX_PATH, full, NULL);

	return length && length < MAX_PATH ? std::wstring(full, length) : path;
}

// What the preprocessor takes for space within a directive
static size_t skip_blank(const char *text, size_t pos, size_t size)
{
	for (;;) {
		if (pos < size && (text[pos] == ' ' || text[pos] == '\t')) {
			pos++;
		} else if (pos + 1 < size && text[pos] == '/' && text[pos + 1] == '*') {
			for (pos += 2; pos + 1 < size && (text[pos] != '*' || text[pos + 1] != '/'); pos++);
			pos = pos + 1 < size ? pos + 2 : size;
		} else {
			return pos;
		}
	}
}

// The compiler looks for an #include "" next to the file that has it and for
// an #include <> not even there, and does not tell its include handler which
// file that was. 3DMigoto takes both forms alike and looks next to the file
// with recursive_include, and without it where D3DCompile's standard include
// handler does: next to the shader itself, then in the working directory.
// Neither can be decided in the handler, so the name in each #include of a
// file is replaced by the path of the file that gives before the compiler
// reads it. The lines stay where they were.
static std::string resolve_includes(const char *text, size_t size, const std::wstring &dir, bool working_dir)
{
	std::string resolved;
	size_t line, pos, name, end, copied = 0;

	for (line = 0; line < size; line = pos + 1) {
		const char *newline;
		size_t line_end;
		std::wstring file, found;
		bool include;

		name = skip_blank(text, line, size);
		include = name < size && text[name] == '#';
		if (include) {
			name = skip_blank(text, name + 1, size);
			include = size - name >= 7 && !memcmp(text + name, "include", 7);
		}
		if (include)
			name = skip_blank(text, name + 7, size);
		// A comment may have taken this past the line it began on
		newline = (const char*)memchr(text + name, '\n', size - name);
		pos = line_end = newline ? newline - text : size;
		if (!include || name == line_end || (text[name] != '"' && text[name] != '<'))
			continue;
		for (end = name + 1; end < line_end && text[end] != (text[name] == '"' ? '"' : '>'); end++);
		if (end == line_end || end == ++name)
			continue;

		file = widen(std::string(text + name, end - name));
		if (file[0] == L'\\' || file[0] == L'/' || (file.size() > 1 && file[1] == L':'))
			continue;
		found = dir + file;
		if (working_dir && GetFileAttributesW(found.c_str()) == INVALID_FILE_ATTRIBUTES && GetFileAttributesW(file.c_str()) != INVALID_FILE_ATTRIBUTES)
			found = full_path(file);

		// Quoted, for the compiler to take the path as it is
		resolved.append(text + copied, name - 1 - copied);
		resolved += '"' + narrow(found) + '"';
		copied = end + 1;
	}
	resolved.append(text + copied, size - copied);
	return resolved;
}

static std::wstring dir_of(const std::wstring &path)
{
	return path.substr(0, path.find_last_of(L"\\/") + 1);
}

class IncludeHandler : public IDxcIncludeHandler
{
	std::wstring root;
	bool recursive;
public:
	IncludeHandler(const std::wstring &root, bool recursive) : root(root), recursive(recursive) {}

	std::string resolve(const char *text, size_t size, const std::wstring &path)
	{
		return resolve_includes(text, size, recursive ? dir_of(path) : root, !recursive);
	}

	STDMETHOD(QueryInterface)(REFIID riid, void **object)
	{
		if (riid != __uuidof(IUnknown) && riid != __uuidof(IDxcIncludeHandler)) {
			*object = NULL;
			return E_NOINTERFACE;
		}
		*object = this;
		return S_OK;
	}
	// On the stack of the compile that uses it
	STDMETHOD_(ULONG, AddRef)() { return 1; }
	STDMETHOD_(ULONG, Release)() { return 1; }

	STDMETHOD(LoadSource)(LPCWSTR filename, IDxcBlob **source)
	{
		IDxcBlobEncoding *blob = NULL;
		std::string text;
		FILE *file = NULL;
		long size;
		HRESULT hr;

		*source = NULL;
		if (_wfopen_s(&file, filename, L"rb") || !file)
			return E_FAIL;
		fseek(file, 0, SEEK_END);
		size = ftell(file);
		fseek(file, 0, SEEK_SET);
		text.resize(size > 0 ? size : 0);
		if (size > 0 && fread(&text[0], 1, size, file) != (size_t)size) {
			fclose(file);
			return E_FAIL;
		}
		fclose(file);

		text = resolve(text.data(), text.size(), filename);
		hr = utils->CreateBlob(text.data(), (UINT32)text.size(), DXC_CP_UTF8, &blob);
		if (FAILED(hr))
			return hr;
		*source = blob;
		return S_OK;
	}
};

bool Dx12DxcCompile(const char *source, size_t size, const wchar_t *path, const wchar_t *target,
		const D3D_SHADER_MACRO *macros, UINT compile_flags, ID3DBlob **bytecode, std::string *errors)
{
	std::vector<std::wstring> owned;
	std::vector<const wchar_t*> args;
	std::wstring file = full_path(path);
	// -1 is D3DCompile's own include handler, which looks where 0 does
	IncludeHandler includes(dir_of(file), G->recursive_include > 0);
	std::string resolved = includes.resolve(source, size, file);
	DxcBuffer buffer = { resolved.data(), resolved.size(), DXC_CP_UTF8 };
	IDxcResult *result = NULL;
	IDxcBlobUtf8 *text = NULL;
	IDxcBlob *object = NULL;
	HRESULT hr, status = E_FAIL;

	*bytecode = NULL;
	errors->clear();

	AcquireSRWLockExclusive(&lock);
	if (!load()) {
		ReleaseSRWLockExclusive(&lock);
		return false;
	}

	args.push_back(L"-E");
	args.push_back(L"main");
	args.push_back(L"-T");
	args.push_back(target);
	// The language D3DCompile took: the HLSL 3DMigoto decompiles selects
	// with a vector condition, which later versions want select() for
	args.push_back(L"-HV");
	args.push_back(L"2016");
	// For what the compiler says of it
	args.push_back(file.c_str());
	for (; macros && macros->Name; macros++) {
		std::wstring define(macros->Name, macros->Name + strlen(macros->Name));

		// Without the = the macro would be 1, and D3DCompile leaves it empty
		define += L"=";
		if (macros->Definition)
			define += std::wstring(macros->Definition, macros->Definition + strlen(macros->Definition));
		owned.push_back(L"-D");
		owned.push_back(define);
	}
	if (compile_flags & D3DCOMPILE_DEBUG)
		args.push_back(L"-Zi");
	// Level 2 is the bits of level 0 and 3 together, and level 1 neither
	if (compile_flags & D3DCOMPILE_SKIP_OPTIMIZATION) {
		args.push_back(L"-Od");
	} else {
		switch (compile_flags & D3DCOMPILE_OPTIMIZATION_LEVEL2) {
		case D3DCOMPILE_OPTIMIZATION_LEVEL0: args.push_back(L"-O0"); break;
		case D3DCOMPILE_OPTIMIZATION_LEVEL1: args.push_back(L"-O1"); break;
		case D3DCOMPILE_OPTIMIZATION_LEVEL2: args.push_back(L"-O2"); break;
		case D3DCOMPILE_OPTIMIZATION_LEVEL3: args.push_back(L"-O3"); break;
		}
	}
	if (compile_flags & D3DCOMPILE_PACK_MATRIX_ROW_MAJOR)
		args.push_back(L"-Zpr");
	if (compile_flags & D3DCOMPILE_PACK_MATRIX_COLUMN_MAJOR)
		args.push_back(L"-Zpc");
	if (compile_flags & D3DCOMPILE_WARNINGS_ARE_ERRORS)
		args.push_back(L"-WX");
	if (compile_flags & D3DCOMPILE_ENABLE_BACKWARDS_COMPATIBILITY)
		args.push_back(L"-Gec");
	if (compile_flags & D3DCOMPILE_IEEE_STRICTNESS)
		args.push_back(L"-Gis");
	if (compile_flags & D3DCOMPILE_ALL_RESOURCES_BOUND)
		args.push_back(L"-all-resources-bound");
	for (const std::wstring &arg : owned)
		args.push_back(arg.c_str());

	hr = compiler->Compile(&buffer, args.data(), (UINT32)args.size(), &includes, IID_PPV_ARGS(&result));
	if (SUCCEEDED(hr) && result) {
		result->GetStatus(&status);
		if (SUCCEEDED(result->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&text), NULL)) && text && text->GetStringLength())
			errors->assign(text->GetStringPointer(), text->GetStringLength());
		if (SUCCEEDED(status) && SUCCEEDED(result->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&object), NULL)) && object
		    && SUCCEEDED(D3DCreateBlob(object->GetBufferSize(), bytecode)))
			memcpy((*bytecode)->GetBufferPointer(), object->GetBufferPointer(), object->GetBufferSize());
	} else {
		errors->assign("the compiler did not run");
	}
	if (object)
		object->Release();
	if (text)
		text->Release();
	if (result)
		result->Release();
	ReleaseSRWLockExclusive(&lock);

	return *bytecode != NULL;
}

bool Dx12DxcReflect(const void *bytecode, size_t size, ID3D12ShaderReflection **reflection)
{
	DxcBuffer buffer = { bytecode, size, DXC_CP_ACP };
	HRESULT hr;

	*reflection = NULL;
	AcquireSRWLockExclusive(&lock);
	hr = load() ? utils->CreateReflection(&buffer, IID_PPV_ARGS(reflection)) : E_FAIL;
	ReleaseSRWLockExclusive(&lock);
	return SUCCEEDED(hr) && *reflection;
}

// -----------------------------------------------------------------------------
// Compiling a shader again to fit the stage before it

struct Origin {
	std::wstring path, target;
	std::vector<std::pair<std::string, std::string>> macros;
	UINT compile_flags;
};

static SRWLOCK origins_lock = SRWLOCK_INIT;
// By the fnv hash of the bytecode, as ShimShader::hash has it
static std::unordered_map<UINT64, Origin> origins;

void Dx12DxcRemember(const void *bytecode, size_t size, const wchar_t *path, const wchar_t *target,
		const D3D_SHADER_MACRO *macros, UINT compile_flags)
{
	Origin origin;

	origin.path = path;
	origin.target = target;
	for (; macros && macros->Name; macros++)
		origin.macros.emplace_back(macros->Name, macros->Definition ? macros->Definition : "");
	origin.compile_flags = compile_flags;

	AcquireSRWLockExclusive(&origins_lock);
	origins[fnv_64_buf(bytecode, size)] = origin;
	ReleaseSRWLockExclusive(&origins_lock);
}

// What the entry point is to take for a semantic in place of what the mod
// declared: this many components, 0 for none at all
struct Fit {
	std::string name;
	UINT index;
	UINT components;
};

static bool same_semantic(const char *a, const char *b)
{
	return !_stricmp(a, b);
}

// Reads a parameter of the entry point: [modifiers] type name : SEMANTIC[index]
struct Parameter {
	std::string text;
	std::string modifiers, type, name, semantic;
	UINT components, index;
	bool out;
};

static std::string trim(const std::string &text)
{
	size_t first = text.find_first_not_of(" \t\r\n"), last = text.find_last_not_of(" \t\r\n");

	return first == std::string::npos ? "" : text.substr(first, last - first + 1);
}

static bool parse_parameter(const std::string &text, Parameter *parameter)
{
	size_t colon = text.find(':'), pos, end;
	std::vector<std::string> words;
	std::string word;

	*parameter = {};
	parameter->text = text;
	if (colon == std::string::npos)
		return false;
	parameter->semantic = trim(text.substr(colon + 1));
	for (pos = 0; pos < colon; pos = end) {
		pos = text.find_first_not_of(" \t\r\n", pos);
		if (pos == std::string::npos || pos >= colon)
			break;
		end = text.find_first_of(" \t\r\n", pos);
		if (end == std::string::npos || end > colon)
			end = colon;
		words.push_back(text.substr(pos, end - pos));
	}
	if (words.size() < 2 || parameter->semantic.empty())
		return false;
	parameter->name = words.back();
	parameter->type = words[words.size() - 2];
	for (size_t i = 0; i + 2 < words.size(); i++) {
		if (words[i] == "out" || words[i] == "inout")
			parameter->out = true;
		else
			parameter->modifiers += words[i] + " ";
	}
	// float4 and the like; a matrix or a struct is left alone
	end = parameter->type.find_last_not_of("0123456789");
	if (end == std::string::npos || end + 1 == parameter->type.size())
		parameter->components = 1;
	else if (end + 2 == parameter->type.size())
		parameter->components = atoi(parameter->type.c_str() + end + 1);
	else
		return false;
	parameter->type = parameter->type.substr(0, end + 1);
	// The index, if any, ends the semantic
	end = parameter->semantic.find_last_not_of("0123456789");
	if (end == std::string::npos)
		return false;
	parameter->index = end + 1 < parameter->semantic.size() ? atoi(parameter->semantic.c_str() + end + 1) : 0;
	parameter->semantic = parameter->semantic.substr(0, end + 1);
	return true;
}

// Rewrites the parameters of main so that each of fits is declared with that
// many components, or not at all, and the name the body uses is made up from
// what is declared with zeros for the rest
static bool fit_source(std::string *source, const std::vector<Fit> &fits, std::string *why)
{
	size_t main_pos = 0, open, close, body, depth, pos, start;
	std::vector<Parameter> parameters;
	std::vector<bool> done(fits.size());
	std::string rebuilt, declarations;
	bool first = true;

	for (;;) {
		main_pos = source->find("main", main_pos);
		if (main_pos == std::string::npos) {
			*why = "its entry point main was not found";
			return false;
		}
		if ((!main_pos || !(isalnum((unsigned char)(*source)[main_pos - 1]) || (*source)[main_pos - 1] == '_'))
		    && source->find_first_not_of(" \t\r\n", main_pos + 4) != std::string::npos
		    && (*source)[source->find_first_not_of(" \t\r\n", main_pos + 4)] == '(')
			break;
		main_pos += 4;
	}
	open = source->find('(', main_pos);
	for (close = open + 1, depth = 1; close < source->size() && depth; close++) {
		if ((*source)[close] == '(')
			depth++;
		else if ((*source)[close] == ')')
			depth--;
	}
	if (depth) {
		*why = "the parameters of main were not found";
		return false;
	}
	close--;
	body = source->find('{', close);
	if (body == std::string::npos) {
		*why = "the body of main was not found";
		return false;
	}

	for (pos = start = open + 1, depth = 0; pos <= close; pos++) {
		char c = (*source)[pos];

		if (c == '(' || c == '<')
			depth++;
		else if (c == ')' && pos < close || c == '>')
			depth--;
		else if ((c == ',' && !depth) || pos == close) {
			Parameter parameter;

			parse_parameter(trim(source->substr(start, pos - start)), &parameter);
			if (!parameter.text.empty())
				parameters.push_back(parameter);
			start = pos + 1;
		}
	}

	rebuilt = source->substr(0, open + 1);
	for (Parameter &parameter : parameters) {
		std::string text = parameter.text;
		size_t i;

		for (i = 0; i < fits.size(); i++) {
			if (parameter.out || parameter.semantic.empty() || !same_semantic(parameter.semantic.c_str(), fits[i].name.c_str())
			    || parameter.index != fits[i].index)
				continue;
			done[i] = true;
			if (fits[i].components >= parameter.components)
				break;
			if (!fits[i].components) {
				text.clear();
				declarations += "  " + parameter.type + (parameter.components > 1 ? std::to_string(parameter.components) : "")
					+ " " + parameter.name + " = 0;\n";
			} else {
				std::string type = parameter.type + std::to_string(parameter.components);

				text = parameter.modifiers + parameter.type + (fits[i].components > 1 ? std::to_string(fits[i].components) : "")
					+ " " + parameter.name + "_fit : " + parameter.semantic + std::to_string(parameter.index);
				declarations += "  " + type + " " + parameter.name + " = " + type + "(" + parameter.name + "_fit";
				for (UINT c = fits[i].components; c < parameter.components; c++)
					declarations += ", 0";
				declarations += ");\n";
			}
			break;
		}
		if (text.empty())
			continue;
		rebuilt += first ? "\n  " : ",\n  ";
		rebuilt += text;
		first = false;
	}
	for (size_t i = 0; i < fits.size(); i++) {
		if (!done[i]) {
			*why = "a semantic the compiled shader reads was not found among the parameters of main";
			return false;
		}
	}
	rebuilt += source->substr(close, body - close + 1) + "\n" + declarations + source->substr(body + 1);
	*source = rebuilt;
	return true;
}

struct Fitted {
	int result;
	std::string why;
	std::vector<BYTE> bytecode;
	Dx12ShaderInfo info;
};

static std::map<std::pair<UINT64, UINT64>, Fitted> fitted_shaders;

int Dx12DxcFitPixelShader(const Dx12ShaderInfo *ps, const Dx12ShaderInfo *upstream, const Dx12ShaderInfo **fitted, const char **why)
{
	std::vector<Dx12SignatureElement> inputs, outputs;
	std::vector<Fit> fits;
	std::vector<D3D_SHADER_MACRO> macros;
	std::string source, errors, cut;
	Origin origin;
	ID3DBlob *blob = NULL;
	FILE *file = NULL;
	long size;
	bool known;

	*fitted = NULL;
	*why = NULL;
	if (!ps->bytecode || !upstream->bytecode)
		return 0;

	AcquireSRWLockExclusive(&origins_lock);
	auto made = fitted_shaders.find(std::make_pair(ps->hash, upstream->hash));
	if (made != fitted_shaders.end()) {
		Fitted *entry = &made->second;

		if (entry->result > 0)
			*fitted = &entry->info;
		else if (entry->result < 0)
			*why = entry->why.c_str();
		ReleaseSRWLockExclusive(&origins_lock);
		return entry->result;
	}
	Fitted *entry = &fitted_shaders[std::make_pair(ps->hash, upstream->hash)];
	auto found = origins.find(ps->hash);
	known = found != origins.end();
	if (known)
		origin = found->second;
	ReleaseSRWLockExclusive(&origins_lock);

	if (!Dx12ReadSignature(ps->bytecode, ps->size, false, &inputs) || !Dx12ReadSignature(upstream->bytecode, upstream->size, true, &outputs)) {
		entry->why = "the signatures of the shaders could not be read";
		goto failed;
	}
	for (const Dx12SignatureElement &input : inputs) {
		const Dx12SignatureElement *output = NULL;
		Fit fit = { input.name, input.index, 0 };

		for (const Dx12SignatureElement &candidate : outputs) {
			if (same_semantic(candidate.name, input.name) && candidate.index == input.index)
				output = &candidate;
		}
		// What the rasterizer makes up is not written upstream
		if (!output && !_strnicmp(input.name, "SV_", 3) && !same_semantic(input.name, "SV_InstanceID"))
			continue;
		if (output && !(input.mask & ~output->mask))
			continue;
		if (output) {
			// Components from x up, as a declaration gives them
			if (output->mask & (output->mask + 1)) {
				entry->why = "the stage before writes components of a semantic the shader reads that no declaration gives";
				goto failed;
			}
			for (UINT mask = output->mask; mask; mask >>= 1)
				fit.components++;
		}
		fits.push_back(fit);
		cut += (cut.empty() ? "" : ", ") + fit.name + std::to_string(fit.index) + " to " + std::to_string(fit.components);
	}
	if (fits.empty()) {
		entry->result = 0;
		return 0;
	}
	if (!known) {
		entry->why = "where the shader was compiled from is not known";
		goto failed;
	}

	if (_wfopen_s(&file, origin.path.c_str(), L"rb") || !file) {
		entry->why = "its source could not be read";
		goto failed;
	}
	fseek(file, 0, SEEK_END);
	size = ftell(file);
	fseek(file, 0, SEEK_SET);
	source.resize(size > 0 ? size : 0);
	if (size <= 0 || fread(&source[0], 1, size, file) != (size_t)size) {
		fclose(file);
		entry->why = "its source could not be read";
		goto failed;
	}
	fclose(file);

	if (!fit_source(&source, fits, &entry->why))
		goto failed;
	for (const auto &macro : origin.macros)
		macros.push_back({ macro.first.c_str(), macro.second.c_str() });
	macros.push_back({ NULL, NULL });
	if (!Dx12DxcCompile(source.data(), source.size(), origin.path.c_str(), origin.target.c_str(), macros.data(),
			origin.compile_flags, &blob, &errors)) {
		entry->why = "compiling it cut down failed: " + errors;
		goto failed;
	}

	entry->bytecode.assign((const BYTE*)blob->GetBufferPointer(), (const BYTE*)blob->GetBufferPointer() + blob->GetBufferSize());
	blob->Release();
	entry->info = *ps;
	entry->info.hash = fnv_64_buf(entry->bytecode.data(), entry->bytecode.size());
	entry->info.size = (UINT32)entry->bytecode.size();
	entry->info.bytecode = entry->bytecode.data();
	entry->result = 1;
	Dx12DumpNote("A mod's pixel shader %016llx was compiled again as %016llx with its inputs cut down to what the game's %s %016llx writes: %s\n",
		ps->hash, entry->info.hash, dx12_stage_names[upstream->stage], upstream->hash, cut.c_str());
	*fitted = &entry->info;
	return 1;

failed:
	entry->result = -1;
	*why = entry->why.c_str();
	return -1;
}
