# MCPX APU Emulation Library
# Extracted from xemu (https://github.com/xemu-project/xemu)
#
# Voice Processor (VP): ADPCM/PCM decode, sample mixing, envelopes, filters
# Global Processor (GP): Stub - effects bypass
# Encode Processor (EP): Stub - direct mixbin passthrough

set(SAMPLERATE_SOURCES
    third_party/samplerate/samplerate.c
    third_party/samplerate/src_sinc.c
    third_party/samplerate/src_linear.c
    third_party/samplerate/src_zoh.c
)
set_source_files_properties(${SAMPLERATE_SOURCES} PROPERTIES
    COMPILE_DEFINITIONS HAVE_CONFIG_H)

add_library(xbox_apu STATIC
    apu_core.c
    apu_vp.c
    apu_dsp.c
    dsp/dsp.c
    dsp/dsp_cpu.c
    dsp/dsp_dma.c
    apu_mmio_hook.c
    apu_xaudio2.c
    ${SAMPLERATE_SOURCES}
)

target_include_directories(xbox_apu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# qemu_shim.h lives in nv2a/ - shared QEMU type abstraction
target_include_directories(xbox_apu PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../nv2a
)

if(MSVC)
    target_compile_options(xbox_apu PRIVATE
        /W3
        /wd4244  # conversion warnings (float/double/int)
        /wd4146  # unary minus on unsigned
        /wd4267  # size_t to int conversion
    )
    target_compile_definitions(xbox_apu PRIVATE
        _CRT_SECURE_NO_WARNINGS
        _USE_MATH_DEFINES
    )
endif()

if(WIN32)
    # xaudio2_8 for XAudio2Create, which apu_xaudio2.c calls directly.
    # It was missing: nothing had ever referenced this library from a linked
    # target, so the archive was built and never pulled in, and the first
    # target to actually call into the APU failed at link with an undefined
    # XAudio2Create rather than at build time here.
    target_link_libraries(xbox_apu PRIVATE platform kernel32 winmm xaudio2_8 ole32 avrt)
else()
    # Linux: platform layer (Win32 primitives on POSIX) + SDL2 audio + libm
    find_package(SDL2 REQUIRED)
    target_link_libraries(xbox_apu PRIVATE platform SDL2::SDL2 m)
endif()
