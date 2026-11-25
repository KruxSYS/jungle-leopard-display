// Product version and publisher for the native binaries' VERSIONINFO.
// Keep JL_VERSION_* in step with <Version> in JLDisplayManager.csproj:
// tools/release.ps1 refuses to build if the two disagree.
#pragma once

#define JL_VERSION_MAJOR 1
#define JL_VERSION_MINOR 7
#define JL_VERSION_PATCH 0

#define JL_STR2(x) #x
#define JL_STR(x) JL_STR2(x)
#define JL_VERSION_STRING JL_STR(JL_VERSION_MAJOR) "." JL_STR(JL_VERSION_MINOR) "." JL_STR(JL_VERSION_PATCH)
#define JL_VERSION_RC JL_VERSION_MAJOR, JL_VERSION_MINOR, JL_VERSION_PATCH, 0

#define JL_COMPANY   "KruxSYS"
#define JL_PRODUCT   "Jungle Leopard Display"
#define JL_COPYRIGHT "\251 2026 KruxSYS"
