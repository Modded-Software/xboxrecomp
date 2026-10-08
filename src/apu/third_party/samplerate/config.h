/*
 * Hand-written config.h for the vendored libsamplerate 0.2.2 build.
 *
 * Upstream generates this with CMake; this project only builds with the
 * zig/mingw (Windows LLP64) and gcc/clang (LP64) toolchains, and only needs
 * the SRC_SINC_FASTEST converter used by xemu's APU VP.
 */

#define PACKAGE "libsamplerate"
#define VERSION "0.2.2"

/* x86/x86_64 does not clip out-of-range float-to-int conversions. */
#define CPU_CLIPS_NEGATIVE 0
#define CPU_CLIPS_POSITIVE 0

#define CPU_IS_BIG_ENDIAN 0
#define CPU_IS_LITTLE_ENDIAN 1

#define HAVE_LRINT 1
#define HAVE_LRINTF 1
#define HAVE_STDBOOL_H 1
#define HAVE_STDINT_H 1

/* Only the fast sinc converter is required (xemu uses SRC_SINC_FASTEST). */
#define ENABLE_SINC_FAST_CONVERTER 1

#define SIZEOF_INT 4
#define SIZEOF_LONG 4