add_executable(ambient_selftest selftest.cpp)
target_link_libraries(ambient_selftest PRIVATE NoctuaryCore)
if(MSVC)
    target_compile_options(ambient_selftest PRIVATE /W4 /permissive- /utf-8)
    target_compile_definitions(ambient_selftest PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
# Several Engines live on the stack at once in some tests (an Engine is a few hundred KB; since the
# oversamplers of 25.09.2026 somewhat more, and 16 MB no longer held them -- 64 MB now), and
# that is true of every compiler on Windows -- see Tools/render/CMakeLists.txt.
if(WIN32)
    target_link_options(ambient_selftest PRIVATE "LINKER:/STACK:67108864")
endif()
add_test(NAME selftest COMMAND ambient_selftest)

# The handovers between the rendering thread and the one that answers the mouse, under load. No
# framework, so it builds on Linux too -- which is the only place a sanitizer can watch a data
# race happen (see the file's own notes). On Windows it is a stress test.
add_executable(ambient_racetest racetest.cpp)
target_link_libraries(ambient_racetest PRIVATE NoctuaryCore)
if(MSVC)
    target_compile_options(ambient_racetest PRIVATE /W4 /permissive- /utf-8)
    target_compile_definitions(ambient_racetest PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
if(WIN32)
    target_link_options(ambient_racetest PRIVATE "LINKER:/STACK:67108864")
endif()
add_test(NAME racetest COMMAND ambient_racetest 4)

# The convolution room's checks on their own, once per vector path: as the core is built for this
# machine (AVX2 on the desktop, NEON on the Quest), and on x86 also through the NEON path -- with
# neonshim/arm_neon.h, which spells the few intrinsics out lane by lane -- and scalar. The two x86
# variants compile the convolver themselves: linking NoctuaryCore would bring its AVX2 flags along.
add_executable(ambient_convtest convtest.cpp)
target_link_libraries(ambient_convtest PRIVATE NoctuaryCore)
# The Quest build is arm64 and must get NEON. (It cannot run here: see Quest/README.md for the
# two adb lines that run it on the headset.)
if(ANDROID AND CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64")
    target_compile_definitions(ambient_convtest PRIVATE AMBIENT_EXPECT_PATH="neon")
endif()
add_test(NAME convtest COMMAND ambient_convtest)
set(AMBIENT_CONVTEST_TARGETS ambient_convtest)
if(NOT ANDROID AND CMAKE_SYSTEM_PROCESSOR MATCHES "(x86_64|AMD64|x64)")
    # neon_nofma takes the plain multiply-add a 32-bit ARM without FMA would, which no real build here reaches.
    foreach(variant neon neon_nofma scalar)
        add_executable(ambient_convtest_${variant} convtest.cpp ${CMAKE_SOURCE_DIR}/Core/src/Convolution.cpp)
        target_include_directories(ambient_convtest_${variant} PRIVATE ${CMAKE_SOURCE_DIR}/Core/include)
        target_compile_features(ambient_convtest_${variant} PRIVATE cxx_std_20)
        add_test(NAME convtest_${variant} COMMAND ambient_convtest_${variant})
        list(APPEND AMBIENT_CONVTEST_TARGETS ambient_convtest_${variant})
    endforeach()
    target_include_directories(ambient_convtest_neon BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/neonshim)
    target_include_directories(ambient_convtest_neon_nofma BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/neonshim)
    target_compile_definitions(ambient_convtest_neon PRIVATE AMBIENT_NEON_SHIM=1 AMBIENT_EXPECT_PATH="neon-shim")
    target_compile_definitions(ambient_convtest_neon_nofma PRIVATE AMBIENT_NEON_SHIM=1 AMBIENT_NEON_SHIM_NOFMA=1 AMBIENT_EXPECT_PATH="neon-shim-nofma")
    target_compile_definitions(ambient_convtest_scalar PRIVATE AMBIENT_EXPECT_PATH="scalar")
endif()
foreach(t ${AMBIENT_CONVTEST_TARGETS})
    if(MSVC)
        target_compile_options(${t} PRIVATE /W4 /permissive- /utf-8)
        target_compile_definitions(${t} PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
endforeach()

# The partial bank's checks, once per vector path, the same way the convolver's are. The bank is a
# header (Core/include/ambient/Simd.h), so the variants need no second source file -- only the
# right defines and, for the shim, the fake <arm_neon.h> in front of the real include path.
add_executable(ambient_banktest banktest.cpp)
target_include_directories(ambient_banktest PRIVATE ${CMAKE_SOURCE_DIR}/Core/include)
target_compile_features(ambient_banktest PRIVATE cxx_std_20)
if(ANDROID AND CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64")
    target_compile_definitions(ambient_banktest PRIVATE AMBIENT_EXPECT_PATH="neon")
endif()
add_test(NAME banktest COMMAND ambient_banktest)
set(AMBIENT_BANKTEST_TARGETS ambient_banktest)
if(NOT ANDROID AND CMAKE_SYSTEM_PROCESSOR MATCHES "(x86_64|AMD64|x64)")
    # The AVX2 one needs the flag in its own right: this target does not link NoctuaryCore.
    target_compile_definitions(ambient_banktest PRIVATE AMBIENT_EXPECT_PATH="avx2")
    if(MSVC)
        target_compile_options(ambient_banktest PRIVATE /arch:AVX2)
    else()
        target_compile_options(ambient_banktest PRIVATE -mavx2)
    endif()
    foreach(variant neon scalar)
        add_executable(ambient_banktest_${variant} banktest.cpp)
        target_include_directories(ambient_banktest_${variant} PRIVATE ${CMAKE_SOURCE_DIR}/Core/include)
        target_compile_features(ambient_banktest_${variant} PRIVATE cxx_std_20)
        add_test(NAME banktest_${variant} COMMAND ambient_banktest_${variant})
        list(APPEND AMBIENT_BANKTEST_TARGETS ambient_banktest_${variant})
    endforeach()
    target_include_directories(ambient_banktest_neon BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/neonshim)
    target_compile_definitions(ambient_banktest_neon PRIVATE AMBIENT_NEON_SHIM=1 AMBIENT_EXPECT_PATH="neon-shim")
    target_compile_definitions(ambient_banktest_scalar PRIVATE AMBIENT_EXPECT_PATH="scalar")
endif()
# The circuit filters' and the oversamplers' checks, once per vector path (26.09.2026): the circuits
# run both channels in the lanes of one register (CircuitFilter.h: NEON on the Quest, SSE here,
# four floats elsewhere) and the oversamplers' sums are vector sums (Oversample.h). The core's own
# build here is SSE lanes and AVX2 sums; the two x86 variants compile the filter themselves, one
# through the NEON shim and one scalar, and the Android build is the real NEON path.
add_executable(ambient_filtertest filtertest.cpp)
target_link_libraries(ambient_filtertest PRIVATE NoctuaryCore)
if(ANDROID AND CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64")
    target_compile_definitions(ambient_filtertest PRIVATE AMBIENT_EXPECT_PATH="neon/neon")
elseif(AMBIENT_AVX2 AND CMAKE_SYSTEM_PROCESSOR MATCHES "(x86_64|AMD64|x64)")
    target_compile_definitions(ambient_filtertest PRIVATE AMBIENT_EXPECT_PATH="sse/avx2")
endif()
add_test(NAME filtertest COMMAND ambient_filtertest)
set(AMBIENT_FILTERTEST_TARGETS ambient_filtertest)
if(NOT ANDROID AND CMAKE_SYSTEM_PROCESSOR MATCHES "(x86_64|AMD64|x64)")
    foreach(variant neon scalar)
        add_executable(ambient_filtertest_${variant} filtertest.cpp ${CMAKE_SOURCE_DIR}/Core/src/Filter.cpp)
        target_include_directories(ambient_filtertest_${variant} PRIVATE ${CMAKE_SOURCE_DIR}/Core/include)
        target_compile_features(ambient_filtertest_${variant} PRIVATE cxx_std_20)
        add_test(NAME filtertest_${variant} COMMAND ambient_filtertest_${variant})
        list(APPEND AMBIENT_FILTERTEST_TARGETS ambient_filtertest_${variant})
    endforeach()
    target_include_directories(ambient_filtertest_neon BEFORE PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/neonshim)
    target_compile_definitions(ambient_filtertest_neon PRIVATE AMBIENT_NEON_SHIM=1 AMBIENT_EXPECT_PATH="neon/neon")
    target_compile_definitions(ambient_filtertest_scalar PRIVATE AMBIENT_SCALAR_LANES=1 AMBIENT_EXPECT_PATH="scalar/scalar")
endif()
foreach(t ${AMBIENT_BANKTEST_TARGETS} ${AMBIENT_FILTERTEST_TARGETS})
    if(MSVC)
        target_compile_options(${t} PRIVATE /W4 /permissive- /utf-8)
        target_compile_definitions(${t} PRIVATE _CRT_SECURE_NO_WARNINGS)
    endif()
endforeach()

# The convolution room's cost, by hand: CPU per second of audio and the worst callback, for impulse
# lengths up to a minute. Nothing in it passes or fails, so it is not registered with ctest.
add_executable(ambient_convbench convbench.cpp)
target_link_libraries(ambient_convbench PRIVATE NoctuaryCore)
if(MSVC)
    target_compile_options(ambient_convbench PRIVATE /W4 /permissive- /utf-8)
    target_compile_definitions(ambient_convbench PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

# The host contract: rates and block sizes, state round trip, programs while playing, an editor
# opened under load, parameters written from another thread. Needs the plugin's shared code, so it
# is only built when the plugin is.
if(TARGET Noctuary)
    add_executable(ambient_hosttest hosttest.cpp)
    # The plugin's shared code carries the JUCE modules and their include paths; linking the
    # wrapper target alone gives neither.
    # The plugin's shared code holds the processor and editor; the JUCE modules are linked to it
    # privately, so the test asks for them again.
    # juce_add_plugin makes Noctuary the shared-code library itself (it builds as
    # Noctuary_SharedCode.lib), so linking that one target gives the processor, the editor and
    # the JucePlugin_* definitions; the module target adds the JUCE headers, which the plugin links
    # privately and therefore does not pass on.
    target_link_libraries(ambient_hosttest PRIVATE Noctuary NoctuaryCore
                          juce::juce_audio_utils juce::juce_recommended_config_flags)
    # The test is an application, not a plugin, so it may run a dispatch loop; that is how the
    # editor's timers and async calls get a chance to run while it is open.
    target_compile_definitions(ambient_hosttest PRIVATE JUCE_STANDALONE_APPLICATION=1
                               JUCE_WEB_BROWSER=0 JUCE_USE_CURL=0 JUCE_DISPLAY_SPLASH_SCREEN=0 JUCE_REPORT_APP_USAGE=0)
    target_include_directories(ambient_hosttest PRIVATE ${CMAKE_SOURCE_DIR}/Plugin)
    if(MSVC)
        target_compile_options(ambient_hosttest PRIVATE /W4 /permissive- /utf-8)
        target_compile_definitions(ambient_hosttest PRIVATE _CRT_SECURE_NO_WARNINGS)
        target_link_options(ambient_hosttest PRIVATE "LINKER:/STACK:67108864")
    endif()
    add_test(NAME hosttest COMMAND ambient_hosttest)
endif()
