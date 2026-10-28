/* Build-time config for the FOS-hosted TinyCC. Searched before third_party/tinycc. */
#pragma once

#define TCC_VERSION "0.9.28"
#define TCC_TARGET_X86_64 1
#define TCC_TARGET_FOS 1

/* Private dir on the boot volume. '{B}' in the paths below. */
#define CONFIG_TCCDIR "\\FOS\\TCC"
#define CONFIG_TCC_SYSINCLUDEPATHS "{B}/include"
#define CONFIG_TCC_LIBPATHS "{B}/lib"
#define CONFIG_TCC_CRTPREFIX "{B}/lib"
#define TCC_LIBTCC1 "libtcc1.a"

#define CONFIG_TCC_SEMLOCK 0
#define CONFIG_TCC_BACKTRACE 0
#define CONFIG_TCC_BCHECK 0
#define CONFIG_TCC_PREDEFS 0
#define CONFIG_TCC_STATIC 1
