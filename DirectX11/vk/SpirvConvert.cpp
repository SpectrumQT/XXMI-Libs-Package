#include "SpirvConvert.h"

#include <algorithm>
#include <fstream>
#include <map>
#include <memory>

#include "VkDump.h"
#include "../IniHandler.h"
#include "../globals.h"

ShaderRegexGroups vk_converted_regex_groups;

namespace {

class Regex {
public:
	explicit Regex(const std::string &pattern, uint32_t options = 0)
	{
		int error;
		PCRE2_SIZE offset;

		code = pcre2_compile((PCRE2_SPTR)pattern.c_str(), pattern.size(), options, &error, &offset, NULL);
	}
	~Regex()
	{
		if (code)
			pcre2_code_free(code);
	}
	Regex(const Regex&) = delete;
	Regex &operator=(const Regex&) = delete;

	bool valid() const { return code != NULL; }

	// Where it matches from the start (or anywhere), with the named groups
	bool match(const std::string &subject, bool anchored, std::map<std::string, std::string> *named = NULL) const
	{
		pcre2_match_data *data;
		bool found;

		if (!code)
			return false;
		data = pcre2_match_data_create_from_pattern(code, NULL);
		found = pcre2_match(code, (PCRE2_SPTR)subject.c_str(), subject.size(), 0, anchored ? PCRE2_ANCHORED : 0, data, NULL) >= 0;
		if (found && named) {
			uint32_t count, entry_size;
			PCRE2_SPTR table;
			PCRE2_SIZE *ovector = pcre2_get_ovector_pointer(data);

			pcre2_pattern_info(code, PCRE2_INFO_NAMECOUNT, &count);
			pcre2_pattern_info(code, PCRE2_INFO_NAMEENTRYSIZE, &entry_size);
			pcre2_pattern_info(code, PCRE2_INFO_NAMETABLE, &table);
			for (uint32_t i = 0; i < count; i++, table += entry_size) {
				unsigned group = table[0] << 8 | table[1];
				if (ovector[2 * group] != PCRE2_UNSET)
					(*named)[(const char*)table + 2] = subject.substr(ovector[2 * group], ovector[2 * group + 1] - ovector[2 * group]);
			}
		}
		pcre2_match_data_free(data);
		return found;
	}

private:
	pcre2_code *code;
};

std::string trim(const std::string &text)
{
	size_t first = text.find_first_not_of(" \t\r\n");
	size_t last = text.find_last_not_of(" \t\r\n");
	return first == text.npos ? "" : text.substr(first, last - first + 1);
}

// Lines without blank lines, comments and the literal \n that ends
// replacement lines, with runs of white space made one space
std::vector<std::string> content(const std::vector<std::string> &lines)
{
	std::vector<std::string> out;

	for (const std::string &raw : lines) {
		std::string line = trim(raw), single;

		if (line.empty() || line[0] == ';')
			continue;
		if (line.size() >= 2 && !line.compare(line.size() - 2, 2, "\\n"))
			line = trim(line.substr(0, line.size() - 2));
		for (char c : line) {
			bool space = c == ' ' || c == '\t';
			if (space && !single.empty() && single.back() == ' ')
				continue;
			single += space ? ' ' : c;
		}
		out.push_back(single);
	}
	return out;
}

std::string join(const std::vector<std::string> &lines, const char *with)
{
	std::string out;

	for (size_t i = 0; i < lines.size(); i++)
		out += (i ? with : "") + lines[i];
	return out;
}

std::string lower(std::string text)
{
	std::transform(text.begin(), text.end(), text.begin(), ::tolower);
	return text;
}

std::string narrow(const std::wstring &text)
{
	return std::string(text.begin(), text.end());
}

void replace_all(std::string *text, const std::string &what, const std::string &with)
{
	for (size_t at = text->find(what); at != text->npos; at = text->find(what, at + with.size()))
		text->replace(at, what.size(), with);
}

// As Python's repr of the float: whole numbers without a fraction
std::string number(const std::string &text)
{
	double value = atof(text.c_str());
	char buf[64];

	if (value == (double)(long long)value)
		return std::to_string((long long)value);
	// The shortest that reads back as the same value
	for (int precision = 1; precision <= 17; precision++) {
		snprintf(buf, sizeof(buf), "%.*g", precision, value);
		if (strtod(buf, NULL) == value)
			break;
	}
	return buf;
}

// The (?=...) parts of a D3D11 pattern that are in no other, as written
std::vector<std::string> lookahead_texts(const std::string &pattern)
{
	std::vector<std::string> parts;

	for (size_t i = pattern.find("(?="); i != pattern.npos; i = pattern.find("(?=", i)) {
		size_t j = i;
		int depth = 0;

		while (j < pattern.size()) {
			if (pattern[j] == '\\') {
				j += 2;
				continue;
			}
			depth += pattern[j] == '(' ? 1 : pattern[j] == ')' ? -1 : 0;
			j++;
			if (!depth)
				break;
		}
		if (j > pattern.size())
			j = pattern.size();
		parts.push_back(pattern.substr(i, j - i));
		i = j;
	}
	return parts;
}

struct Group {
	const ShaderRegexSource *source;
	std::string shader_model;

	explicit Group(const ShaderRegexSource *source) : source(source)
	{
		auto main = source->sections.find(L"");
		if (main == source->sections.end())
			return;
		for (const std::string &line : main->second) {
			size_t equals = line.find('=');
			if (equals != line.npos && lower(trim(line.substr(0, equals))) == "shader_model")
				shader_model = lower(trim(line.substr(equals + 1)));
		}
	}

	const std::vector<std::string> &section(const wchar_t *name) const
	{
		static const std::vector<std::string> none;
		auto found = source->sections.find(name);
		return found == source->sections.end() ? none : found->second;
	}

	bool spirv() const
	{
		return std::any_of(source->sections.begin(), source->sections.end(), [](const auto &entry) {
			return !entry.first.compare(0, 12, L"spirvpattern"); });
	}

	std::vector<std::wstring> replacements() const
	{
		std::vector<std::wstring> out;

		for (const auto &entry : source->sections) {
			if (entry.first.size() >= 8 && !entry.first.compare(entry.first.size() - 8, 8, L".replace"))
				out.push_back(entry.first);
		}
		return out;
	}
};

// A recipe knows one kind of D3D11 patch: match() gives the values that tell
// its uses apart, or none, and write() the SPIR-V version of those.
class Recipe {
public:
	virtual ~Recipe() {}
	virtual const char *name() const = 0;
	virtual bool match(const Group &group, std::vector<std::string> *key) const = 0;
	virtual std::vector<std::string> write(const std::string &name, const std::vector<std::string> &key,
			const std::vector<std::string> &sources) const = 0;
};

std::string sources_list(const std::vector<std::string> &sources)
{
	std::string out;

	for (const std::string &source : sources)
		out += (out.empty() ? "[" : ", [") + source + "]";
	return out;
}

// CutoutMask.ini (Zhuang Fangyi Ink Cheongsam, Liino Moonlit Violet and their
// copies): where an IniParams value equals a mod's id, a mask texture is read at
// the texel under TEXCOORD and the pixel is discarded where a channel of it is
// below a threshold.
class CutoutMask : public Recipe {
public:
	const char *name() const override { return "cutout mask"; }

	bool match(const Group &group, std::vector<std::string> *key) const override
	{
		static const Regex body(
			R"(\$\{header\}\n)"
			R"(ld_indexable\(texture1d\)\(float,float,float,float\) \$\{(?P<i>\w+)\}\.xyzw, l\((?P<index>\d+), 0\), t(?P<ini>\d+)\.xyzw\n)"
			R"(eq \$\{(?P=i)\}\.x, \$\{(?P=i)\}\.x, l\((?P<id>[-0-9.]+)\)\n)"
			R"(if_nz \$\{(?P=i)\}\.x\n)"
			R"(resinfo_indexable\(texture2d\)\(float,float,float,float\) \$\{(?P<s>\w+)\}\.xy, l\(0\), t(?P<mask>\d+)\.xyzw\n)"
			R"(add \$\{(?P=s)\}\.z, \$\{(?P=s)\}\.x, \$\{(?P=s)\}\.y\n)"
			R"(if_nz \$\{(?P=s)\}\.z\n)"
			R"(frc \$\{(?P<c>\w+)\}\.xy, v(?P<input>\d+)\.xyxx\n)"
			R"(mul \$\{(?P=c)\}\.xy, \$\{(?P=c)\}\.xyxx, \$\{(?P=s)\}\.xyxx\n)"
			R"(ftou \$\{(?P=c)\}\.xy, \$\{(?P=c)\}\.xyxx\n)"
			R"(mov \$\{(?P=c)\}\.zw, l\(0, 0, 0, 0\)\n)"
			R"(ld_indexable\(texture2d\)\(float,float,float,float\) \$\{(?P<m>\w+)\}\.(?P<channel>[xyzw]), \$\{(?P=c)\}\.xyzz, t(?P=mask)\.xyzw\n)"
			R"(lt \$\{(?P=m)\}\.x, \$\{(?P=m)\}\.(?P=channel), l\((?P<threshold>[-0-9.]+)\)\n)"
			R"(discard_nz \$\{(?P=m)\}\.x\n)"
			R"(endif\n)"
			R"(endif\z)");
		std::map<std::string, std::string> m;

		if (group.shader_model.compare(0, 3, "ps_") || group.replacements() != std::vector<std::wstring>{ L"pattern.replace" })
			return false;
		std::string pattern = join(group.section(L"pattern"), "");
		if (pattern.find("(?<header>") == pattern.npos)
			return false;
		if (!body.match(join(content(group.section(L"pattern.replace")), "\n"), true, &m))
			return false;
		if (content(group.section(L"insertdeclarations")) != std::vector<std::string>{
				"dcl_resource_texture1d (float,float,float,float) t" + m["ini"],
				"dcl_resource_texture2d (float,float,float,float) t" + m["mask"] })
			return false;
		// SV_Position is v0 in these shaders and a BuiltIn in SPIR-V, which
		// numbers the other inputs from 0
		int input = atoi(m["input"].c_str());
		if (input < 1)
			return false;
		// Whether the pattern takes shaders where that input is a float2, a
		// float4 or both
		std::string widths;
		for (int width : { 2, 4 }) {
			std::string line = "dcl_input_ps linear v" + m["input"] + "." + std::string("xyzw", width) + "\n";
			bool all = true;
			for (const std::string &part : lookahead_texts(pattern)) {
				if (part.find("dcl_input_ps") == part.npos)
					continue;
				Regex inner(part.substr(3, part.size() - 4));
				all = all && inner.valid() && inner.match(line, false);
			}
			if (all)
				widths += std::to_string(width);
		}
		if (widths.empty())
			return false;
		*key = { m["ini"], m["index"], number(m["id"]), m["mask"], std::to_string(input - 1),
			std::to_string(std::string("xyzw").find(m["channel"])), number(m["threshold"]), widths };
		return true;
	}

	std::vector<std::string> write(const std::string &name, const std::vector<std::string> &key,
			const std::vector<std::string> &sources) const override
	{
		const std::string &ini = key[0], &index = key[1], &mod_id = key[2], &mask = key[3], &location = key[4],
			&channel = key[5], &threshold = key[6], &widths = key[7];
		std::vector<std::string> out = { "; " + sources_list(sources) + ": where IniParams x" + index + " is " + mod_id
			+ " and a texture is at ps-t" + mask + ", discards where its " + "xyzw"[atoi(channel.c_str())]
			+ " at the TEXCOORD at Location " + location + " is below " + threshold + "." };

		for (char width : widths) {
			std::string group = "ShaderRegex" + name + "V" + width;
			std::vector<std::string> uv;
			if (width == '2') {
				uv = { "${uv} = OpLoad %zz_v2float %zz_in_loc" + location + "\\n" };
			} else {
				uv = { "${uv4} = OpLoad %zz_v4float %zz_in_loc" + location + "\\n",
					"${uv} = OpVectorShuffle %zz_v2float ${uv4} ${uv4} 0 1\\n" };
			}
			std::vector<std::string> lines = {
				"",
				"[" + group + "]",
				"shader_model = ps_5_0",
				"temps = c0 cindex cid cthreshold pini ini inix on check img size sx sy sum bound fetch uv uv4 frc"
				" sizef scaled coord texel value cut kill end end2 end3",
				"",
				"[" + group + ".InsertDeclarations]",
				"dcl_resource_texture1d (float,float,float,float) t" + ini,
				"dcl_resource_texture2d (float,float,float,float) t" + mask,
				"",
				"[" + group + ".SpirvPatternA]",
				"^%zz_in_loc" + location + " = OpVariable %zz_ptr_Input_v" + width + "float Input$",
				"",
				"[" + group + ".SpirvPatternB]",
				"^@zz_globals$",
				"",
				"[" + group + ".SpirvPatternB.Replace]",
				"${c0} = OpConstant %zz_int 0\\n",
				"${cindex} = OpConstant %zz_int " + index + "\\n",
				"${cid} = OpConstant %zz_float " + mod_id + "\\n",
				"${cthreshold} = OpConstant %zz_float " + threshold + "\\n",
				"@zz_globals",
				"",
				"[" + group + ".SpirvPatternC]",
				"^@zz_entry$",
				"",
				"[" + group + ".SpirvPatternC.Replace]",
				"${pini} = OpAccessChain %zz_ptr_Uniform_v4float %IniParams ${c0} ${cindex}\\n",
				"${ini} = OpLoad %zz_v4float ${pini}\\n",
				"${inix} = OpCompositeExtract %zz_float ${ini} 0\\n",
				"${on} = OpFOrdEqual %zz_bool ${inix} ${cid}\\n",
				"OpSelectionMerge ${end} None\\n",
				"OpBranchConditional ${on} ${check} ${end}\\n",
				"${check} = OpLabel\\n",
				"${img} = OpLoad %zz_image2d_float %t" + mask + "\\n",
				"${size} = OpImageQuerySizeLod %zz_v2int ${img} ${c0}\\n",
				"${sx} = OpCompositeExtract %zz_int ${size} 0\\n",
				"${sy} = OpCompositeExtract %zz_int ${size} 1\\n",
				"${sum} = OpIAdd %zz_int ${sx} ${sy}\\n",
				"${bound} = OpINotEqual %zz_bool ${sum} ${c0}\\n",
				"OpSelectionMerge ${end2} None\\n",
				"OpBranchConditional ${bound} ${fetch} ${end2}\\n",
				"${fetch} = OpLabel\\n",
			};
			lines.insert(lines.end(), uv.begin(), uv.end());
			lines.insert(lines.end(), {
				"${frc} = OpExtInst %zz_v2float %zz_glsl Fract ${uv}\\n",
				"${sizef} = OpConvertSToF %zz_v2float ${size}\\n",
				"${scaled} = OpFMul %zz_v2float ${frc} ${sizef}\\n",
				"${coord} = OpConvertFToS %zz_v2int ${scaled}\\n",
				"${texel} = OpImageFetch %zz_v4float ${img} ${coord} Lod ${c0}\\n",
				"${value} = OpCompositeExtract %zz_float ${texel} " + channel + "\\n",
				"${cut} = OpFOrdLessThan %zz_bool ${value} ${cthreshold}\\n",
				"OpSelectionMerge ${end3} None\\n",
				"OpBranchConditional ${cut} ${kill} ${end3}\\n",
				"${kill} = OpLabel\\n",
				"OpKill\\n",
				"${end3} = OpLabel\\n",
				"OpBranch ${end2}\\n",
				"${end2} = OpLabel\\n",
				"OpBranch ${end}\\n",
				"${end} = OpLabel\\n",
				"@zz_entry",
			});
			out.insert(out.end(), lines.begin(), lines.end());
		}
		return out;
	}
};

// [ShaderRegexSmoothNormalColor] of Si FGO lancer Melusine and its copies:
// where an IniParams value equals a mod's id, the outline vertex shader takes
// the normal it makes of the vertex colour, as it does when the material's
// flag for that (cb3[N].x > 0.5) is on.
class SmoothNormalColor : public Recipe {
public:
	const char *name() const override { return "smooth normal colour"; }

	bool match(const Group &group, std::vector<std::string> *key) const override
	{
		static const Regex body(
			R"(\$\{prefix\}ld_indexable\(texture1d\)\(float,float,float,float\) \$\{(?P<g>\w+)\}\.xyzw, )"
			R"(l\((?P<index>\d+), 0, 0, 0\), t(?P<ini>\d+)\.xyzw\n)"
			R"(eq \$\{(?P=g)\}\.x, \$\{(?P=g)\}\.x, l\((?P<id>[-0-9.]+)\)\n)"
			R"(or \$\{flag\}\.\$\{lane\}, \$\{flag\}\.\$\{lane\}, \$\{(?P=g)\}\.x\z)");
		// What the D3D11 pattern has to find: the flag, then the normal made
		// of the colour's xy and chosen by the flag
		static const char *const needed[] = {
			R"((?P<prefix>.*?^lt (?P<flag>r\d+)\.(?P<lane>[xyzw]), l\(0\.500000\), cb3\[)",
			R"(dp2 (?P<length>(?P<length_reg>r\d+)\.(?P<length_lane>[xyzw])), (?P=color)\.xyxx, (?P=color)\.xyxx\n)",
			R"(sqrt (?P=length), (?P=length)\n)",
			R"(movc (?P<selected>r\d+)\.xyz, (?P=flag)\.(?P=lane){4}, (?P=normal)\.xyzx, (?P=base_normal)\n)",
		};
		std::map<std::string, std::string> m;

		if (group.shader_model.compare(0, 3, "vs_") || group.replacements() != std::vector<std::wstring>{ L"pattern.replace" })
			return false;
		std::string pattern = join(group.section(L"pattern"), "");
		// The same groups written the way Python names them
		std::string normalized;
		for (size_t i = 0; i < pattern.size(); i++) {
			if (!pattern.compare(i, 3, "(?<") && i + 3 < pattern.size() && pattern[i + 3] != '=' && pattern[i + 3] != '!') {
				normalized += "(?P<";
				i += 2;
			} else if (!pattern.compare(i, 3, "\\k<")) {
				size_t end = pattern.find('>', i);
				if (end == pattern.npos) {
					normalized += pattern.substr(i);
					break;
				}
				normalized += "(?P=" + pattern.substr(i + 3, end - i - 3) + ")";
				i = end;
			} else {
				normalized += pattern[i];
			}
		}
		for (const char *part : needed) {
			if (normalized.find(part) == normalized.npos)
				return false;
		}
		std::string replace = join(content(group.section(L"pattern.replace")), "\n");
		replace_all(&replace, "\\n", "\n");
		if (!body.match(trim(replace), true, &m))
			return false;
		if (content(group.section(L"insertdeclarations")) != std::vector<std::string>{
				"dcl_resource_texture1d (float,float,float,float) t" + m["ini"] })
			return false;
		// Which shaders, by what they declare: the same on Vulkan, where the
		// DLL writes the declarations a D3D11 shader would have
		std::string declared;
		for (const std::string &part : lookahead_texts(pattern)) {
			if (part.find("dcl_") != part.npos)
				declared += part;
		}
		if (!Regex("(?ms)" + declared).valid())
			return false;
		*key = { m["ini"], m["index"], number(m["id"]), declared };
		return true;
	}

	std::vector<std::string> write(const std::string &name, const std::vector<std::string> &key,
			const std::vector<std::string> &sources) const override
	{
		const std::string &ini = key[0], &index = key[1], &mod_id = key[2], &declared = key[3];
		std::string group = "ShaderRegex" + name;
		std::vector<std::string> out = {
			"; " + sources_list(sources) + ": where IniParams x" + index + " is " + mod_id
				+ ", the outline vertex shader takes the normal made of the vertex colour as if the material's flag for it were on.",
			"",
			"[" + group + "]",
			"shader_model = vs_5_0",
			"temps = c0 cindex cid was pini ini inix gate",
			"",
		};
		if (!declared.empty())
			out.insert(out.end(), { "[" + group + ".Pattern]", R"((?ms)\A)" + declared, "" });
		out.insert(out.end(), {
			"[" + group + ".InsertDeclarations]",
			"dcl_resource_texture1d (float,float,float,float) t" + ini,
			"",
			"[" + group + ".SpirvPatternA]",
			"^@zz_globals$",
			"",
			"[" + group + ".SpirvPatternA.Replace]",
			"${c0} = OpConstant %zz_int 0\\n",
			"${cindex} = OpConstant %zz_int " + index + "\\n",
			"${cid} = OpConstant %zz_float " + mod_id + "\\n",
			"@zz_globals",
			"",
			// The flag is a float of a uniform block compared to 0.5, and
			// chooses a block that makes the normal of the colour's xy as
			// (x, y, sqrt(1 - saturate(dot(xy, xy))))
			"[" + group + ".SpirvPatternB]",
			R"((?s)\A(?=.*?^(?<half>%\d+) = OpConstant %zz_float 0\.5$))",
			R"((?<prefix>.*?^(?<pw>%\d+) = OpAccessChain %zz_ptr_Uniform_float %\d+ %\d+\n)"
			R"((?<width>%\d+) = OpLoad %zz_float \k<pw>\n))",
			R"((?<flag>%\d+) = OpFOrdGreaterThan %zz_bool \k<width> \k<half>\n)",
			R"((?=OpSelectionMerge (?<merge>%\d+) None\nOpBranchConditional \k<flag> (?<smooth>%\d+) \k<merge>\n)"
			R"(\k<smooth> = OpLabel\n)",
			R"((?<x>%\d+) = OpCompositeExtract %zz_float (?<color>%\d+) 0\n)"
			R"((?<y>%\d+) = OpCompositeExtract %zz_float \k<color> 1\n)",
			R"((?<v>%\d+) = OpCompositeConstruct %zz_v3float \k<x> \k<y> %\d+\n)"
			R"((?<xy>%\d+) = OpVectorShuffle %zz_v2float \k<v> \k<v> 0 1\n)",
			R"((?<dot>%\d+) = OpDot %zz_float \k<xy> \k<xy>\n)"
			R"((?<sat>%\d+) = OpExtInst %zz_float %zz_glsl FClamp \k<dot> %\d+ %\d+\n)",
			R"((?<rest>%\d+) = OpFSub %zz_float %\d+ \k<sat>\n%\d+ = OpExtInst %zz_float %zz_glsl Sqrt \k<rest>\n))",
			"",
			"[" + group + ".SpirvPatternB.Replace]",
			"${prefix}${was} = OpFOrdGreaterThan %zz_bool ${width} ${half}\\n",
			"${pini} = OpAccessChain %zz_ptr_Uniform_v4float %IniParams ${c0} ${cindex}\\n",
			"${ini} = OpLoad %zz_v4float ${pini}\\n",
			"${inix} = OpCompositeExtract %zz_float ${ini} 0\\n",
			"${gate} = OpFOrdEqual %zz_bool ${inix} ${cid}\\n",
			"${flag} = OpLogicalOr %zz_bool ${was} ${gate}\\n",
		});
		return out;
	}
};

const Recipe *const recipes[] = { new CutoutMask(), new SmoothNormalColor() };

std::wstring folder_of(const std::wstring &file)
{
	size_t slash = file.find_last_of(L"\\/");
	return slash == file.npos ? L"" : file.substr(0, slash);
}

bool same_path(const std::wstring &a, const std::wstring &b)
{
	return !_wcsicmp(a.c_str(), b.c_str());
}

// The groups of the ini lines a recipe wrote, the way IniHandler.cpp reads
// them, under that prefix. Adds none if any fails.
bool build_groups(const SpirvConverted &converted, ShaderRegexGroups *groups, std::string *why)
{
	struct Written {
		std::map<std::wstring, std::string> main;
		std::vector<std::string> declarations;
		std::map<std::wstring, std::string> patterns, replacements;
	};
	std::map<std::wstring, Written> written;
	Written *group = NULL;
	std::wstring subsection;

	for (const std::string &raw : converted.lines) {
		std::string line = trim(raw);

		if (line.empty() || line[0] == ';')
			continue;
		if (line[0] == '[' && line.back() == ']') {
			std::wstring section(line.begin() + 1, line.end() - 1);
			size_t dot = section.find(L'.');
			std::wstring name = section.substr(0, dot);

			if (_wcsnicmp(name.c_str(), L"ShaderRegex", 11)) {
				*why = "a section of the conversion is not a ShaderRegex one: " + line;
				return false;
			}
			group = &written[converted.prefix + name.substr(11)];
			subsection = dot == section.npos ? L"" : section.substr(dot + 1);
			continue;
		}
		if (!group) {
			*why = "a line of the conversion is in no section: " + line;
			return false;
		}
		std::wstring lower_sub = subsection;
		std::transform(lower_sub.begin(), lower_sub.end(), lower_sub.begin(), ::towlower);
		if (subsection.empty()) {
			size_t equals = line.find('=');
			if (equals != line.npos) {
				std::string key = lower(trim(line.substr(0, equals)));
				group->main[std::wstring(key.begin(), key.end())] = trim(line.substr(equals + 1));
			}
		} else if (lower_sub == L"insertdeclarations") {
			group->declarations.push_back(line);
		} else if (lower_sub.size() > 8 && !lower_sub.compare(lower_sub.size() - 8, 8, L".replace")) {
			group->replacements[subsection.substr(0, subsection.size() - 8)] += line;
		} else {
			group->patterns[subsection] += line;
		}
	}

	auto words = [](const std::string &text) {
		std::set<std::string> out;
		size_t start = 0;

		while (start < text.size()) {
			size_t end = text.find(' ', start);
			if (end == text.npos)
				end = text.size();
			if (end > start)
				out.insert(text.substr(start, end - start));
			start = end + 1;
		}
		return out;
	};

	auto fail = [&](const std::string &reason) {
		for (auto &entry : written)
			groups->erase(entry.first);
		*why = reason;
		return false;
	};

	for (auto &entry : written) {
		if (groups->count(entry.first))
			return fail("a group of that name is there already: " + narrow(entry.first));
	}
	for (auto &entry : written) {
		ShaderRegexGroup &made = (*groups)[entry.first];

		made.ini_section = entry.first;
		made.shader_models = words(entry.second.main[L"shader_model"]);
		made.temp_regs = words(entry.second.main[L"temps"]);
		for (const std::string &line : entry.second.declarations) {
			SpirvDeclaration declaration;
			if (!parse_spirv_declaration(line, G->IniParamsReg, &declaration))
				return fail("not a declaration for SPIR-V: " + line);
			made.spirv_declarations.push_back(declaration);
		}
		for (auto &pattern : entry.second.patterns) {
			bool spirv = !_wcsnicmp(pattern.first.c_str(), L"SpirvPattern", 12);
			ShaderRegexPattern &compiled = spirv ? made.spirv_patterns[pattern.first] : made.patterns[pattern.first];
			if (!compiled.compile(&pattern.second))
				return fail("a pattern of the conversion does not compile: " + pattern.second);
			auto replace = entry.second.replacements.find(pattern.first);
			if (replace != entry.second.replacements.end()) {
				compiled.replace = replace->second;
				compiled.do_replace = true;
			}
		}
	}
	return true;
}

}

void SpirvConvertSources(const std::vector<ShaderRegexSource> &sources, const std::set<std::wstring> &files_with_vulkan_sections,
		std::vector<SpirvConverted> *converted, std::vector<std::pair<std::wstring, std::string>> *not_converted)
{
	std::vector<std::wstring> files, spirv_folders;

	converted->clear();
	not_converted->clear();
	for (const ShaderRegexSource &source : sources) {
		if (std::none_of(files.begin(), files.end(), [&](const std::wstring &file) { return same_path(file, source.file); }))
			files.push_back(source.file);
		if (Group(&source).spirv())
			spirv_folders.push_back(folder_of(source.file));
	}

	for (const std::wstring &file : files) {
		struct Found {
			const Recipe *recipe;
			std::vector<std::string> key;
			std::vector<std::wstring> from;
			std::wstring prefix;
		};
		std::vector<Found> found;
		std::vector<const ShaderRegexSource*> patches;
		bool own_sections = std::any_of(files_with_vulkan_sections.begin(), files_with_vulkan_sections.end(),
			[&](const std::wstring &other) { return same_path(other, file); });
		bool own_spirv = std::any_of(spirv_folders.begin(), spirv_folders.end(),
			[&](const std::wstring &folder) { return same_path(folder, folder_of(file)); });

		for (const ShaderRegexSource &source : sources) {
			Group group(&source);
			if (same_path(source.file, file) && !group.replacements().empty() && !group.spirv())
				patches.push_back(&source);
		}
		for (const ShaderRegexSource *source : patches) {
			Group group(source);
			std::wstring id = source->prefix + source->name;
			std::vector<std::string> key;
			const Recipe *recipe = NULL;

			if (own_sections) {
				not_converted->push_back({ id, "its ini has sections of its own for Vulkan" });
				continue;
			}
			if (own_spirv) {
				not_converted->push_back({ id, "its folder has a SPIR-V version of its own" });
				continue;
			}
			for (const Recipe *candidate : recipes) {
				if (candidate->match(group, &key)) {
					recipe = candidate;
					break;
				}
			}
			if (!recipe) {
				not_converted->push_back({ id, "no conversion knows this patch" });
				continue;
			}
			auto same = std::find_if(found.begin(), found.end(), [&](const Found &other) {
				return other.recipe == recipe && other.key == key; });
			if (same == found.end()) {
				found.push_back({ recipe, key, {}, source->prefix });
				same = found.end() - 1;
			}
			same->from.push_back(source->name);
		}
		for (const Found &one : found) {
			SpirvConverted made;
			std::vector<std::string> names;

			for (const std::wstring &name : one.from)
				names.push_back("ShaderRegex" + narrow(name));
			made.prefix = one.prefix;
			made.recipe = one.recipe->name();
			for (const std::wstring &name : one.from)
				made.from.push_back(one.prefix + name);
			made.lines = one.recipe->write(narrow(one.from[0]) + "Vulkan", one.key, names);
			converted->push_back(std::move(made));
		}
	}
}

void VkConvertShaderRegexGroups()
{
	std::vector<ShaderRegexSource> sources;
	std::set<std::wstring> vulkan_files;
	std::vector<SpirvConverted> converted;
	std::vector<std::pair<std::wstring, std::string>> not_converted;
	ShaderRegexGroups groups;
	LARGE_INTEGER start, end, frequency;

	QueryPerformanceCounter(&start);
	GetShaderRegexSources(&sources, &vulkan_files);
	SpirvConvertSources(sources, vulkan_files, &converted, &not_converted);

	for (const SpirvConverted &one : converted) {
		std::string why, from, to;

		for (const std::wstring &id : one.from)
			from += (from.empty() ? "[" : ", [") + narrow(id) + "]";
		if (!build_groups(one, &groups, &why)) {
			VkDumpNote("*** ShaderRegex: %s could not be converted for Vulkan: %s\n", from.c_str(), why.c_str());
			continue;
		}
		for (const std::string &line : one.lines) {
			if (line.size() > 2 && line[0] == '[' && line.back() == ']' && line.find('.') == line.npos)
				to += (to.empty() ? "[" : ", [") + line.substr(1, line.size() - 2) + "]";
		}
		VkDumpNote("ShaderRegex: %s is used for Vulkan as %s (%s)\n", from.c_str(), to.c_str(), one.recipe.c_str());
	}
	for (const auto &one : not_converted)
		VkDumpNote("ShaderRegex: the D3D11 patch [%s] is not used for Vulkan: %s\n", narrow(one.first).c_str(), one.second.c_str());
	QueryPerformanceCounter(&end);
	QueryPerformanceFrequency(&frequency);
	if (!sources.empty())
		VkDumpNote("ShaderRegex: %u groups looked at for the conversion for Vulkan in %.2f ms\n",
			(unsigned)sources.size(), (end.QuadPart - start.QuadPart) * 1000.0 / frequency.QuadPart);

	ShaderRegexConfigUpdate update;
	vk_converted_regex_groups.swap(groups);
}

// For local-patches/spirv-regex: the conversion of the D3D11 groups of one
// ini file, written to out_path as the ini it would be, with what was not
// converted as comments. Returns how many groups it wrote, -1 if the file
// could not be read.
extern "C" __declspec(dllexport) int VkSpirvConvertIniFile(const wchar_t *ini_path, const wchar_t *out_path)
{
	std::ifstream file(ini_path, std::ios::binary);
	std::vector<ShaderRegexSource> sources;
	std::set<std::wstring> vulkan_files;
	std::vector<SpirvConverted> converted;
	std::vector<std::pair<std::wstring, std::string>> not_converted;
	std::map<std::wstring, size_t> by_name;
	std::string raw, ns;
	std::vector<std::string> *section = NULL;
	int written = 0;

	if (!file)
		return -1;
	while (std::getline(file, raw)) {
		std::string line = trim(raw);

		if (line.empty() || line[0] == ';')
			continue;
		if (line[0] == '[' && line.back() == ']') {
			std::string name = line.substr(1, line.size() - 2);
			std::string low = lower(name);
			size_t dot = low.find('.');

			if (low.find("vulkan") != low.npos)
				vulkan_files.insert(ini_path);
			section = NULL;
			if (low.compare(0, 11, "shaderregex"))
				continue;
			std::wstring group(name.begin() + 11, name.begin() + (dot == low.npos ? name.size() : dot));
			std::wstring sub(low.begin() + (dot == low.npos ? low.size() : dot + 1), low.end());
			auto found = by_name.find(group);
			if (found == by_name.end()) {
				found = by_name.emplace(group, sources.size()).first;
				sources.emplace_back();
				sources.back().name = group;
				sources.back().file = ini_path;
			}
			section = &sources[found->second].sections[sub];
			continue;
		}
		if (section) {
			section->push_back(line);
		} else if (sources.empty() && !lower(line).compare(0, 9, "namespace")) {
			size_t equals = line.find('=');
			if (equals != line.npos)
				ns = trim(line.substr(equals + 1));
		}
	}
	std::wstring prefix = ns.empty() ? L"ShaderRegex" : L"ShaderRegex\\" + std::wstring(ns.begin(), ns.end()) + L"\\";
	for (ShaderRegexSource &source : sources)
		source.prefix = prefix;

	SpirvConvertSources(sources, vulkan_files, &converted, &not_converted);
	std::ofstream out(out_path, std::ios::binary);
	if (!ns.empty())
		out << "namespace = " << ns << "\n";
	for (const SpirvConverted &one : converted) {
		out << "\n";
		for (const std::string &line : one.lines)
			out << line << "\n";
		written++;
	}
	for (const auto &one : not_converted)
		out << "; not converted: [" << narrow(one.first) << "]: " << one.second << "\n";
	return written;
}
