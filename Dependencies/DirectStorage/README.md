# DirectStorage SDK (vendored)

Headers, import library and runtime DLLs for the Microsoft DirectStorage API
(`DStorageCreateFactory` / `DStorageSetConfiguration`), used by the optional
DirectStorage texture loading in `DirectX11/DirectStorageManager.cpp`.

| File | Origin |
|---|---|
| `include/dstorage.h`, `include/dstorageerr.h` | `Microsoft.Direct3D.DirectStorage` NuGet 1.3.0 (`native/include`) |
| `lib/x64/dstorage.lib` | same package (`native/lib/x64`) |
| `../dstorage.dll`, `../dstoragecore.dll` | same package (`native/bin/x64`, version 1.3.2506.2501) |
| `LICENSE-CODE.txt` | package license for the code portions |

The runtime DLLs in `Dependencies/` are copied to the build output by the
existing post-build `xcopy "$(SolutionDir)Dependencies\*.*"` step. They are
Microsoft redistributables and are also present in System32 on Windows 11.
