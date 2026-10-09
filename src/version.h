// The app's version and identity: the one place they're defined.
// Read by main.cpp (--version, the live page, the
// results page), by app.rc (the exe's version resource, which the
// installer reads in turn), and checked by the tests.
//
// Plain #defines only: the resource compiler (rc.exe) includes this file
// too and understands nothing else.
#pragma once

#define WASD_VERSION_MAJOR 0
#define WASD_VERSION_MINOR 1
#define WASD_VERSION_PATCH 0
#define WASD_VERSION_STRING "0.1.0"

#define WASD_APP_NAME "WASD: World Awareness & State Display"
#define WASD_APP_SHORT_NAME "WASD"  // folders, shortcuts, window classes: no ":" allowed in file names
#define WASD_APP_PUBLISHER "AmishGoose"
#define WASD_APP_DESCRIPTION "WASD: World Awareness & State Display, a read-only companion for Dark Souls III (Seamless Co-op)"
#define WASD_APP_COPYRIGHT "Copyright (C) 2026 AmishGoose"
