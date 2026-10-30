// framework.h : standard system include files for Vault Commander
//

#pragma once

#include "targetver.h"

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#define WIN32_LEAN_AND_MEAN             // Exclude rarely-used stuff from Windows headers
#define NOMINMAX

// Windows Header Files
#include <windows.h>
#include <ole2.h>
#include <shobjidl.h>
#include <shellapi.h>   // SHFileOperation, SHGetFileInfo, ShellExecute (not pulled in by WIN32_LEAN_AND_MEAN)
#include <commctrl.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <commdlg.h>

// C RunTime Header Files
#include <stdlib.h>
#include <malloc.h>
#include <memory.h>
#include <tchar.h>
#include <cstdarg>
#include <cstdio>

// C++ Standard Library
#include <string>
#include <vector>
#include <fstream>
#include <algorithm>

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "uuid.lib")

// Visual styles (modern look of buttons, list view, etc.)
#pragma comment(linker, "\"/manifestdependency:type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
