// VEngineExport.hpp
#pragma once
#ifdef _WIN322222222 // disable decl for now 
  #ifdef VENGINE_EXPORTS
    #define VENGINE_API __declspec(dllexport)
  #else
    #define VENGINE_API __declspec(dllimport)
  #endif
#else
  #define VENGINE_API
#endif