"""Run the SPIR-V ShaderRegex regression in an isolated Vulkan harness directory."""

import argparse
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile

HERE = Path(__file__).parent


def mask_dds(alpha):
    """A 4x4 R8G8B8A8_UNORM texture of that alpha."""
    header = [124, 0x100F, 4, 4, 16, 0, 1] + [0] * 11
    header += [32, 4, int.from_bytes(b"DX10", "little"), 0, 0, 0, 0, 0]
    header += [0x1000, 0, 0, 0, 0]
    return (b"DDS " + struct.pack("<31I", *header) + struct.pack("<5I", 28, 3, 0, 1, 0)
            + bytes([255, 255, 255, alpha]) * 16)


def mod_ini(guid=True, mask=True, vs_guid=False):
    ini = (HERE / "spirv_patch.ini").read_text(encoding="utf-8")
    if vs_guid:
        ini = ini.replace("        x246 = 0\n", "        x246 = 246001\n", 1)
    if not guid:
        ini = ini.replace("        x245 = 245997\n", "        x245 = 1\n", 1)
    if not mask:
        ini = ini.replace("        ps-t116 = ref ResourceMask\n", "", 1)
    return ini


def group_ini(sampler=False):
    ini = (HERE / "spirv_patch_group.ini").read_text(encoding="utf-8")
    if sampler:
        start, end = ini.index("; lookup\n"), ini.index("; end of lookup\n")
        ini = ini[:start] + (
            "${smp} = OpLoad %zz_sampler %s0\\n\n"
            "${sampled} = OpSampledImage %zz_sampled_image2d_float ${img} ${smp}\\n\n"
            "${texel} = OpImageSampleExplicitLod %zz_v4float ${sampled} ${frc} Lod ${fzero}\\n\n"
        ) + ini[end + len("; end of lookup\n"):]
        ini = ini.replace("; sampler\n", "dcl_sampler s0, mode_default\n", 1)
    return ini


def vs_group_ini():
    return (HERE / "spirv_patch_vs_group.ini").read_text(encoding="utf-8")


def unmatched_group_ini():
    """A group that never matches, declaring t0, which the harness's pixel shader has a texture at."""
    return (
        "[ShaderRegexHarnessNever]\n"
        "shader_model = ps_5_0\n"
        "[ShaderRegexHarnessNever.InsertDeclarations]\n"
        "dcl_resource_texture2d (float,float,float,float) t0\n"
        "[ShaderRegexHarnessNever.SpirvPattern]\n"
        "^%no_such_line$\n"
    )


def run_case(folder, mod, group=None, alpha=0, draws=1, system=""):
    (folder / "d3dx.ini").write_text(
        "[Loader]\ntarget = harness.exe\n"
        + (f"[System]\n{system}\n" if system else "") +
        "[Include]\ninclude_recursive = Mods\n"
        "[Rendering]\nextended_rendering = 1\ntrack_region_hashes = 1\n"
        "[Hunting]\nhunting = 0\n"
        "[Logging]\nlog_level = info\nshow_warnings = 0\nunbuffered = 1\n",
        encoding="utf-8",
    )
    mods = folder / "Mods"
    (mods / "test.ini").write_text(mod, encoding="utf-8")
    group_file = mods / "test_vulkan.ini"
    if group is None:
        group_file.unlink(missing_ok=True)
    else:
        group_file.write_text(group, encoding="utf-8")
    (mods / "mask.dds").write_bytes(mask_dds(alpha))
    result = subprocess.run(
        [str(folder / "harness.exe"), "2", "--pool", "--draws", str(draws)],
        cwd=folder,
        capture_output=True,
        text=True,
        timeout=120,
        check=True,
    )
    log = (folder / "d3d11_log.txt").read_text(encoding="utf-8", errors="replace")
    errors = [line for line in (log + result.stdout).splitlines()
              if re.search(r"\bWARNING\b|SYNTAX ERROR|\bFAILED\b|\*\*\* ShaderRegex", line, re.I)
              and not re.search(r", 0 failed$", line)]
    if errors:
        raise AssertionError(f"Harness or INI failed:\n{result.stdout}\n" + "\n".join(errors))
    pixels = re.findall(r"^(middle|corner): (.+)$", result.stdout, re.M)
    if len(pixels) != 2:
        raise AssertionError(f"Missing rendered pixels:\n{result.stdout}")
    return tuple(pixels), log


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--harness-root", type=Path, required=True)
    parser.add_argument("--dll", type=Path, required=True)
    parser.add_argument("--spirv-tools", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="spirv-patch-", dir=args.output) as temp:
        folder = Path(temp).resolve()
        mods = folder / "Mods"
        mods.mkdir()
        for name in ("harness.exe", "vs.spv", "ps.spv", "d3dcompiler_47.dll", "dxcompiler.dll", "dxil.dll"):
            shutil.copyfile(args.harness_root / name, folder / name)
        shutil.copyfile(args.dll, folder / "d3d11.dll")
        shutil.copyfile(args.spirv_tools, folder / "SPIRV-Tools-shared.dll")
        for name in ("tri_vb16.buf", "tri_ib.buf", "fill.hlsl"):
            shutil.copyfile(args.harness_root / "Mods" / name, mods / name)

        game, _ = run_case(folder, "")
        drawn, _ = run_case(folder, mod_ini())
        hidden, _ = run_case(folder, mod_ini().replace(
            "        drawindexedinstanced = 3, INSTANCE_COUNT, 0, 0, FIRST_INSTANCE\n", "", 1))
        if drawn == hidden or drawn == game or hidden == game:
            raise AssertionError(f"The game, the mod's triangle and nothing must differ: {game} {drawn} {hidden}")

        cut, log = run_case(folder, mod_ini(), group_ini())
        if cut != hidden:
            raise AssertionError(f"Where the mask's alpha is 0 the patched shader must discard: expected {hidden}, got {cut}")
        if not re.search(r"is patched by \[[^\]]*HarnessCut\]", log, re.I):
            raise AssertionError("The log must say which section patched the shader")
        print("PASS: a transparent mask with the guid discards the mod's triangle")

        # Then the configuration read when the DLL is loaded is all there is
        unreloaded, _ = run_case(folder, mod_ini(), group_ini(), system="config_initialization_delay = -1")
        if unreloaded != hidden:
            raise AssertionError(f"Without the reload at the start the group must patch as well: expected {hidden}, got {unreloaded}")
        print("PASS: with config_initialization_delay = -1 the group read at the start patches the shader")

        # The harness's pipeline takes "main", which this module has second
        shutil.copyfile(args.harness_root / "ps_two_entries.spv", folder / "ps.spv")
        try:
            two_drawn, _ = run_case(folder, mod_ini())
            two_cut, _ = run_case(folder, mod_ini(), group_ini())
        finally:
            shutil.copyfile(args.harness_root / "ps.spv", folder / "ps.spv")
        if two_drawn != drawn or two_cut != hidden:
            raise AssertionError(f"The entry point the pipeline uses must be patched: expected {drawn} and {hidden}, got {two_drawn} and {two_cut}")
        print("PASS: the patch goes into the entry point the pipeline uses, not the first of the module")

        # One module with a vertex and a fragment "main", for both stages
        for name in ("vs.spv", "ps.spv"):
            shutil.copyfile(args.harness_root / "vs_ps_main.spv", folder / name)
        try:
            groups = group_ini() + "\n" + vs_group_ini()
            one_module = {
                "drawn": run_case(folder, mod_ini())[0],
                "cut": run_case(folder, mod_ini(), groups)[0],
                "moved": run_case(folder, mod_ini(vs_guid=True), groups, alpha=255)[0],
            }
        finally:
            for name in ("vs.spv", "ps.spv"):
                shutil.copyfile(args.harness_root / name, folder / name)
        if one_module != {"drawn": drawn, "cut": hidden, "moved": hidden}:
            raise AssertionError(f"Each stage of one module must be patched for itself: {one_module}, expected drawn {drawn}, hidden {hidden}")
        print("PASS: a module with a vertex and a fragment main is patched for each stage")

        cases = {
            "an opaque mask": run_case(folder, mod_ini(), group_ini(), alpha=255)[0],
            "another guid": run_case(folder, mod_ini(guid=False), group_ini())[0],
            "no mask set": run_case(folder, mod_ini(mask=False), group_ini())[0],
        }
        for name, pixels in cases.items():
            if pixels != drawn:
                raise AssertionError(f"With {name} the triangle must be drawn as without the patch: expected {drawn}, got {pixels}")
        print("PASS: an opaque mask, another guid and no mask leave the triangle as it is")

        sampled, _ = run_case(folder, mod_ini(), group_ini(sampler=True))
        if sampled != hidden:
            raise AssertionError(f"The sampler of InsertDeclarations must sample the mask: expected {hidden}, got {sampled}")
        print("PASS: a sampler declared with dcl_sampler samples the mask")

        beside, _ = run_case(folder, mod_ini(), group_ini() + "\n" + unmatched_group_ini())
        if beside != hidden:
            raise AssertionError(f"A group that does not match must not keep one that does from being used: expected {hidden}, got {beside}")
        print("PASS: what a group that does not match declares does not stop the patch of one that does")

        # The second draw of the frame is the harness's own, after the patched
        # one: it has to be made with the game's bindings
        game_twice, _ = run_case(folder, "", draws=2)
        after, _ = run_case(folder, mod_ini(), group_ini(), draws=2)
        if after != game_twice:
            raise AssertionError(f"A draw of the game after a patched one must be as the game's: expected {game_twice}, got {after}")
        print("PASS: the game's draw after a patched one is made with the game's bindings")

        moved, log = run_case(folder, mod_ini(vs_guid=True), vs_group_ini())
        if moved != hidden:
            raise AssertionError(f"With x246 set the patched vertex shader must move the triangle away: expected {hidden}, got {moved}")
        if not re.search(r"the vertex shader [0-9a-f]+ is patched by \[[^\]]*HarnessMove\]", log, re.I):
            raise AssertionError("The log must say which section patched the vertex shader")
        print("PASS: a vertex shader group without textures is used for the mod's draw")

        unmoved, _ = run_case(folder, mod_ini(), vs_group_ini())
        if unmoved != drawn:
            raise AssertionError(f"Without x246 the patched vertex shader must draw as the game's: expected {drawn}, got {unmoved}")
        print("PASS: without x246 the patched vertex shader draws the triangle where it was")

        groups = group_ini() + "\n" + vs_group_ini()
        both = {
            "cut": run_case(folder, mod_ini(), groups)[0],
            "opaque": run_case(folder, mod_ini(), groups, alpha=255)[0],
            "moved": run_case(folder, mod_ini(vs_guid=True), groups, alpha=255)[0],
        }
        if both != {"cut": hidden, "opaque": drawn, "moved": hidden}:
            raise AssertionError(f"With both groups each must do as it does alone: {both}, expected hidden {hidden}, drawn {drawn}")
        print("PASS: a pipeline with both stages patched cuts and moves as each group alone")

        after_moved, _ = run_case(folder, mod_ini(vs_guid=True), vs_group_ini(), draws=2)
        if after_moved != game_twice:
            raise AssertionError(f"A draw of the game after a patched vertex shader must be as the game's: expected {game_twice}, got {after_moved}")
        print("PASS: the game's draw after one with a patched vertex shader is the game's")

        print("Rendered pixels:", cut, drawn)


if __name__ == "__main__":
    main()
