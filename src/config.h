#ifndef __batyr_config_h__
#define __batyr_config_h__

#include "macros.h"

// version number
#define VERSION_MAJOR 0
#define VERSION_MINOR 1
#define VERSION_PATCH 0
#define VERSION_FULL STRINGIFY(VERSION_MAJOR) "." STRINGIFY(VERSION_MINOR) "." STRINGIFY(VERSION_PATCH)


#define APP_NAME_SERVER         "batyrd"
#define APP_NAME_SERVER_FULL    APP_NAME_SERVER " v" VERSION_FULL

#endif // __batyr_config_h__