#pragma once
#ifdef _MSC_VER
#ifdef LC_HLSL_DLL
#define LC_HLSL_EXTERN __declspec(dllexport) extern
#else
#define LC_HLSL_EXTERN __declspec(dllimport) extern
#endif
#else
#define LC_HLSL_EXTERN extern
#endif