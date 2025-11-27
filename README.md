## Information
A fork/ repository created to bypass a certain error with the goal in mind to bypass it
I will try to build and upload custom dlls here every 2 days to bypass checksums and filehashes checks that the game specifically uses to flag dlls
## Working behind it:
As for why it works, the reason is pretty simple. The certain anime game is build using directx and because of how the api works, the game itself needing the dynamic link libraries and other programs using it
The company behind the game can't really block it


## Installation:
Just paste the d3d11 in your XXMI-Launcher-Portable-v1.9.2\Resources\Packages
folder and click replace if asked
If you get the illegal tool error then simply download the new version of the dll and replace it
Sadly you'd have to keep doing it till I decide to build an application that automatically does it for you



## 3Dmigoto





















![image](https://cloud.githubusercontent.com/assets/6544511/22624161/934dba64-eb27-11e6-8f78-46c902e96e1b.png)
========

**XXMI DLL** is a fork of [3dmigoto](https://github.com/bo3b/3Dmigoto/), a Direct3D modding tool. Developed as part of the **XXMI Project** by the [AGMG Community](https://discord.gg/agmg), it serves as a core component of the [XXMI Launcher](https://github.com/SpectrumQT/XXMI-Launcher). This fork focuses on streamlining the original, enhances compatibility and introduces performance and usability improvements.

## Key differences

* Legacy projects have been removed from the solution.
* All stereoscopic (3D) features have been stripped, making this fork effectively a "2Dmigoto".
* Codebase has been updated for compatibility with **Visual Studio 2022**.

## New features

* Micro-optimizations to reduce config reload times and main loop FPS impact.
* UTF-8 support for file paths and namespaces.
* Configurable buffers resizing support, also known as "vertex limit raise" (backward-compatible with GIMI `.dll` implementation).
* GPU-to-INI scope data transfer support via `store` command (backward-compatible with GIMI `.dll` implementation).
* Periodic auto-saving of mod settings (persistent vars are saved to `d3dx_user.ini` every 1 minute by default).
* Support for remote DLL loading (allows the loader `.exe` to reside in a separate directory).

## Special Thanks

- Chiri, [Bo3b](https://github.com/bo3b), [DarkStarSword](https://github.com/DarkStarSword) — for the monumental work behind the original 3dmigoto.
- [SilentNightSound](https://github.com/SilentNightSound) — for pioneering AGMG D3D modding and introducing foundational GIMI DLL features.
- [SinsOfSeven](https://github.com/SinsOfSeven) — for input on architecture decisions and original source analysis.
- [Nurarihyon](https://github.com/NurarihyonMaou), [Scyll](https://gamebanana.com/members/2644630) — for contributions and optimization insights.
- [Leotorrez](https://github.com/leotorrez) & [Gustav0](https://github.com/Seris0/Gustav0) — for extensive testing and tireless listening to community feedback.
