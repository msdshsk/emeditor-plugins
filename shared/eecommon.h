// 全プラグイン共通のプリアンブル。
// 各プラグインの .cpp で、etlframe.h より前に 1 回だけ include する。
#pragma once

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "comctl32.lib")

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <windowsx.h>
#include <tchar.h>
#include <malloc.h>
#include <crtdbg.h>
#include <commctrl.h>
#include <process.h>
#pragma warning( push )
#pragma warning( disable : 4995 ) // 'function': name was marked as #pragma deprecated
#include <shlwapi.h>
#include <strsafe.h>
#pragma warning( pop )

#include <new>
#include <string>
#include <vector>

#ifdef _DEBUG
#define VERIFY(f)          _ASSERTE(f)
#else
#define VERIFY(f)          ((void)(f))
#endif

// etlframe.h は _DEBUG_NEW が定義されていると Debug ビルドで new を置き換える。
// Release では new(nothrow) にして、確保失敗時に例外ではなく NULL を返す（公式サンプルと同じ方針）。
#ifdef _DEBUG
#define _DEBUG_NEW   new( _NORMAL_BLOCK, __FILE__, __LINE__ )
#define _RELEASE_NEW
#else
#define _DEBUG_NEW
#define _RELEASE_NEW new( std::nothrow )
#endif
