// Generated version information.
// version.generated.h is created by the build system.
// If it does not exist, these fallback values are used.

// Release tag format:
//   vMAJOR.MINOR.PATCH
//   vMAJOR.MINOR.PATCH.BUILD
//
// Examples:
//   v1.4.0   -> 1.4.0.0
//   v1.4.0.7 -> 1.4.0.7

#include "version.generated.h"

#ifndef VERSION_MAJOR
#define VERSION_MAJOR               0
#endif

#ifndef VERSION_MINOR
#define VERSION_MINOR               0
#endif

#ifndef VERSION_REVISION
#define VERSION_REVISION            0
#endif

#ifndef VERSION_BUILD
#define VERSION_BUILD               0
#endif

#ifndef VER_COMPANYNAME_STR
#define VER_COMPANYNAME_STR            ""
#endif

#ifndef VER_COMMENTS_STR
#define VER_COMMENTS_STR            ""
#endif

#define STRINGIZE2(s) #s
#define STRINGIZE(s) STRINGIZE2(s)

#define VER_FILE_VERSION \
    VERSION_MAJOR, VERSION_MINOR, VERSION_REVISION, VERSION_BUILD

#define VER_FILE_VERSION_STR \
    STRINGIZE(VERSION_MAJOR) "." \
    STRINGIZE(VERSION_MINOR) "." \
    STRINGIZE(VERSION_REVISION) "." \
    STRINGIZE(VERSION_BUILD)
 
#define VER_PRODUCTNAME_STR         "XXMI DLL (3Dmigoto)"
#define VER_PRODUCT_VERSION         VER_FILE_VERSION
#define VER_PRODUCT_VERSION_STR     VER_FILE_VERSION_STR
#define VER_COPYRIGHT_STR           "Copyright (C) 2014-2019"

#ifdef _DEBUG
  #define VER_VER_DEBUG             VS_FF_DEBUG
#else
  #define VER_VER_DEBUG             0
#endif
 
#define VER_FILEOS                  VOS_NT_WINDOWS32
#define VER_FILEFLAGS               VER_VER_DEBUG
#define VER_FILETYPE                VFT_DLL

#ifdef APSTUDIO_INVOKED
#ifndef APSTUDIO_READONLY_SYMBOLS
#define _APS_NEXT_RESOURCE_VALUE        101
#define _APS_NEXT_COMMAND_VALUE         40001
#define _APS_NEXT_CONTROL_VALUE         1001
#define _APS_NEXT_SYMED_VALUE           101
#endif
#endif

// Used for embedded font resource for overlay
#define SPRITEFONT					256
#define IDR_COURIERBOLD				101
#define IDR_COURIERSMALL			102
#define IDR_ARIAL				103

