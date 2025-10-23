// VEngineExport.hpp
#pragma once
#if _MSC_VER && !__INTEL_COMPILER
  #ifdef VENGINE_EXPORTS
    #define VENGINE_API __declspec(dllexport)
  #else
    #define VENGINE_API __declspec(dllimport)
  #endif
#else
  #define VENGINE_API
#endif