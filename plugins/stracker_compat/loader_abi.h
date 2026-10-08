#pragma once

// Stracker's Loader ABI, reproduced exactly so existing Stracker plugins bind
// to CMFM's loader.dll unchanged.
//
// Forked from MHWLoader/loader.h of Stracker's Loader
// (https://github.com/Strackeror/MHW-QuestLoader, commit 9770302), MIT,
// Copyright (c) 2020 Remi Lavergne. Its LOG class derives from BSD-2-Clause
// code, Copyright (c) 2018 Philipp Kraehenbuehl. Both notices ship in
// licences/.
//
// Plugins import these symbols; their names and the LOG layout must not
// change:
//   ??0LOG@loader@@QEAA@W4LogLevel@1@@Z    LOG::LOG(LogLevel)
//   ??1LOG@loader@@QEAA@XZ                 LOG::~LOG()
//   ?MinLogLevel@loader@@3W4LogLevel@1@A   LogLevel MinLogLevel
//   ?GameVersion@loader@@3PEBDEB           const char* GameVersion
// A plugin runs its own inline operator<< on the std::stringstream this DLL
// constructs, so the DLL is built like Stracker's: MSVC, release, static CRT.

#include <sstream>

#ifdef CMFM_STRACKER_SHIM
#define CMFM_STRACKER_API __declspec(dllexport)
#else
#define CMFM_STRACKER_API __declspec(dllimport)
#endif

#pragma warning(push)
#pragma warning(disable : 4251)  // an STL member in an exported class: the ABI requires it

namespace loader {
enum LogLevel { DEBUG = 0, INFO = 1, WARN = 2, ERR = 3 };

CMFM_STRACKER_API extern const char* GameVersion;
CMFM_STRACKER_API extern LogLevel MinLogLevel;

class CMFM_STRACKER_API LOG {
private:
    std::stringstream stream;
    LogLevel logLevel;
    LOG(const LOG& o) = delete;
    LOG& operator=(const LOG& o) = delete;

public:
    LOG(LogLevel level) : logLevel(level) {}
    ~LOG();

    template <class T>
    LOG& operator<<(const T& x) {
        if (logLevel >= MinLogLevel) stream << x;
        return *this;
    }
};
}  // namespace loader

#pragma warning(pop)
