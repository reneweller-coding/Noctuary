cmake_minimum_required(VERSION 3.22)
project(Noctuary VERSION 2.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(AMBIENT_BUILD_PLUGIN "Build the JUCE plugin (VST3 + Standalone)" ON)
option(AMBIENT_BUILD_TOOLS  "Build the offline render tool and the self test" ON)
set(AMBIENT_JUCE_TAG "9.0.1" CACHE STRING "JUCE git tag to fetch")

if(MSVC)
    add_compile_options(/MP)
    # Symbols for Release, so a crash can be read. The instrument writes its own stack trace on a
    # crash (Documents/Noctuary/crash.log), and without a .pdb beside the executable that trace
    # is the Windows frames and nothing of ours -- JUCE's backtrace skips every frame it cannot
    # name, so the one part worth seeing is the part that goes missing. /DEBUG is a LINKER switch
    # and changes no code; it does turn the /OPT defaults off, so they are named again here and the
    # binary stays what it was. Guarded on MSVC: the release build runs through the Intel toolchain
    # and is not to be touched.
    add_link_options($<$<CONFIG:Release>:/DEBUG> $<$<CONFIG:Release>:/OPT:REF> $<$<CONFIG:Release>:/OPT:ICF>)
endif()

# A build meant for other people has to run on a machine that has never seen a compiler, so the
# runtime is linked in rather than looked up: no "VCRUNTIME140.dll is missing" for anybody. Off
# for everyday builds, where the shared runtime links faster and debugs better.
option(AMBIENT_STATIC_RUNTIME "Link the MSVC runtime statically (for distribution)" OFF)
if(AMBIENT_STATIC_RUNTIME AND MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Link-time optimisation, off by default: measured on MSVC over a cloud- and sub-heavy render it
# made no difference at all (1.05-1.16 s either way), so it only costs link time here. Worth
# trying on the clang/NDK build. Fast-math is deliberately NOT enabled: the offline render is the
# determinism oracle of the self test, and reassociation makes it drift -- moving one line of
# processBlock once changed a result by 35 % (see docs/concept.md).
option(AMBIENT_LTO "Link-time optimisation in Release" OFF)
if(AMBIENT_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT AMBIENT_IPO OUTPUT AMBIENT_IPO_MSG)
    if(AMBIENT_IPO)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
    else()
        message(STATUS "LTO unavailable: ${AMBIENT_IPO_MSG}")
    endif()
endif()

add_subdirectory(Core)

if(AMBIENT_BUILD_TOOLS)
    enable_testing()
    add_subdirectory(Tools/render)
endif()

if(AMBIENT_BUILD_PLUGIN)
    include(FetchContent)
    set(JUCE_BUILD_EXTRAS OFF CACHE BOOL "" FORCE)
    set(JUCE_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(JUCE
        GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
        GIT_TAG        ${AMBIENT_JUCE_TAG}
        GIT_SHALLOW    TRUE
        GIT_PROGRESS   TRUE)
    FetchContent_MakeAvailable(JUCE)
    add_subdirectory(Plugin)
endif()

# After the plugin, so the host-contract test can link against its shared code.
if(AMBIENT_BUILD_TOOLS)
    add_subdirectory(Tests)
endif()
