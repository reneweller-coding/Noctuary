cmake_minimum_required(VERSION 3.22)
project(Noctuary VERSION 2.6.0 LANGUAGES C CXX)

# The family's layout (01.10.2026): build/<preset>, bin/<preset>, dist/, work/ -- cmake/Family.cmake,
# CMakePresets.json and build.ps1, the same in every repository of the family.
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/Family.cmake)

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
    family_doccheck()   # every entity documented (cmake/Family.cmake, 01.10.2026)
    add_subdirectory(Tools/render)
    family_soundcheck(ambient_render)   # the sound regression check (Tools/soundcheck.py, 02.10.2026)
    # The music audit (20.11.2026): both conductors an hour over a fixed sample of 200 library presets, without audio,
    # measured for the faults found by ear (rubs, doublings, notes struck again, notes shorter than their attack) and
    # checked against Tests/golden/music_audit.json. Labelled slow: about two minutes on eight cores.
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND AND TARGET ambient_brain_audit)
        add_test(NAME music_audit COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/Tools/library/music_audit.py"
                 --check "${CMAKE_SOURCE_DIR}/Tests/golden/music_audit.json" --exe "$<TARGET_FILE:ambient_brain_audit>"
                 --tsv "${CMAKE_BINARY_DIR}/music_audit/check.tsv")
        set_tests_properties(music_audit PROPERTIES LABELS "slow" TIMEOUT 1800)
    endif()
    # The layer banks (20.11.2026, plan C2): the generator's default run, without a taste file, makes exactly the
    # near, Cosmos and Strike banks and the near table in the tree. A few seconds.
    if(Python3_Interpreter_FOUND)
        add_test(NAME layer_presets COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/Tools/make_layer_presets.py" --check)
        # One conductor for the family (20.11.2026, plan A1): Core/include/ambient/ClusterBrain.h is what
        # Tools/family/sync_brain.py makes from the canonical Tools/family/ClusterBrain.h.in, never edited by hand.
        # Under a second.
        add_test(NAME brain_in_sync COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/Tools/family/sync_brain.py"
                 --check "${CMAKE_SOURCE_DIR}/Core/include/ambient/ClusterBrain.h" --product ambient)
        # The night director's table (20.11.2026, plan D3): Core/src/NightDirector.inc is what Tools/make_night.py makes
        # from the library's measurement (work/library/packs.json; skipped where that is not at hand). A few seconds.
        add_test(NAME night_table COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/Tools/make_night.py" --check)
        set_tests_properties(night_table PROPERTIES SKIP_RETURN_CODE 77)
        # The near layer's night guard (20.11.2026, plan C3): Tools/library/near_night.json has a row for every preset
        # of the near bank, Core/src/NearNight.inc is what it makes, and each row's fade and gain keep the preset's
        # measured events inside the limits. The renders themselves are a tool (near_night.py verify). Seconds.
        add_test(NAME near_night COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/Tools/library/near_night.py" --check)
    endif()
endif()

if(AMBIENT_BUILD_PLUGIN)
    family_juce(${AMBIENT_JUCE_TAG})   # ThirdParty/JUCE, the sibling Phosphene's, or fetched (cmake/Family.cmake)
    add_subdirectory(Plugin)
    # Noctuary Journeys: journeys rendered into WAV, FLAC, Ogg Vorbis or MP3, faster than real time (16.10.2026),
    # on the family's render app (Tools/renderapp/RenderKit.h).
    add_subdirectory(Tools/renderapp)
endif()

# After the plugin, so the host-contract test can link against its shared code.
if(AMBIENT_BUILD_TOOLS)
    add_subdirectory(Tests)
endif()
