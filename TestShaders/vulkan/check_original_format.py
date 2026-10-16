"""Run the original-format regression in an isolated Vulkan harness directory."""

import argparse
from pathlib import Path
import re
import shutil
import struct
import subprocess
import tempfile


def run_case(folder, ini):
    (folder / "Mods" / "test.ini").write_text(ini, encoding="utf-8")
    result = subprocess.run(
        [str(folder / "harness.exe"), "1", "--pool"],
        cwd=folder,
        capture_output=True,
        text=True,
        timeout=60,
        check=True,
    )
    log = (folder / "d3d11_log.txt").read_text(encoding="utf-8", errors="replace")
    errors = [line for line in (log + result.stdout).splitlines()
              if re.search(r"\bWARNING\b|SYNTAX ERROR|\bFAILED\b", line, re.I)
              and not re.search(r", 0 failed$", line)]
    if errors:
        raise AssertionError(f"Harness or INI failed:\n{result.stdout}\n" + "\n".join(errors))
    pixels = re.findall(r"^(middle|corner): (.+)$", result.stdout, re.M)
    if len(pixels) != 2:
        raise AssertionError(f"Missing rendered pixels:\n{result.stdout}")
    return tuple(pixels)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--harness-root", type=Path, required=True)
    parser.add_argument("--dll", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="original-format-", dir=args.output) as temp:
        folder = Path(temp).resolve()
        mods = folder / "Mods"
        mods.mkdir()
        for name in ("harness.exe", "vs.spv", "ps.spv", "d3dcompiler_47.dll", "dxcompiler.dll", "dxil.dll"):
            shutil.copyfile(args.harness_root / name, folder / name)
        shutil.copyfile(args.dll, folder / "d3d11.dll")
        for name in ("tri_vb16.buf", "tri_ib.buf", "tex_rgba.dds", "tex_bc1.dds", "fill.hlsl"):
            shutil.copyfile(args.harness_root / "Mods" / name, mods / name)

        # The replacement is never sampled; its format differs from the game's RGBA texture.
        header = [124, 0x1007, 4, 4, 16, 0, 1] + [0] * 11
        header += [32, 4, int.from_bytes(b"DX10", "little"), 0, 0, 0, 0, 0]
        header += [0x1000, 0, 0, 0, 0]
        (mods / "replacement_bc7.dds").write_bytes(
            b"DDS " + struct.pack("<31I", *header) + struct.pack("<5I", 98, 3, 0, 1, 0) + b"\x01" + bytes(15)
        )
        (folder / "d3dx.ini").write_text(
            "[Loader]\ntarget = harness.exe\n"
            "[Include]\ninclude_recursive = Mods\n"
            "[Rendering]\ndx12 = auto\nvulkan = 1\ntrack_region_hashes = 1\n"
            "[Hunting]\nhunting = 0\n"
            "[Logging]\nlog_level = info\nshow_warnings = 0\nunbuffered = 1\n",
            encoding="utf-8",
        )
        ini = Path(__file__).with_name("original_format.ini").read_text(encoding="utf-8")
        branch = "        if $ok\n"
        assert ini.count(branch) == 1
        passing = run_case(folder, ini.replace(branch, "        if 1\n"))
        failing = run_case(folder, ini.replace(branch, "        if 0\n"))
        if passing == failing:
            raise AssertionError("Pass and fail textures must produce different pixels")
        legacy = ini.replace(
            "$ok = $ok && ps->srv_original_format($slot) == 28\n",
            "$ok = $ok && ps-t[$slot]->format == 28\n",
            1,
        )
        assert legacy != ini
        if run_case(folder, legacy) != failing:
            raise AssertionError("The old detector must fail after the format-changing replacement")
        actual = run_case(folder, ini)
        if actual != passing:
            raise AssertionError(f"Original-format contract failed: expected {passing}, got {actual}")
        print("PASS: the old detector fails after replacement; original-format detection passes")
        print("PASS: current format, dynamic slot, clearing, missing slots/stages and outside-draw queries")
        print("Rendered pixels:", actual)


if __name__ == "__main__":
    main()
