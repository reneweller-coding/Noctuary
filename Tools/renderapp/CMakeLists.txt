# Noctuary Journeys (16.10.2026): the family's render app (RenderKit.h, the same in every instrument; canonical in Empyrean)
# with Noctuary's source -- its journeys (the templates and the player's own), rendered into WAV, FLAC, Ogg Vorbis or MP3, faster than real time, each shown with
# its length.
set(_app NoctuaryJourneys)
juce_add_gui_app(${_app}
    COMPANY_NAME     "Rene Weller"
    BUNDLE_ID        "com.reneweller.noctuary.journeys"
    PRODUCT_NAME     "${_app}"
    VERSION          ${PROJECT_VERSION}
    ICON_BIG         "${CMAKE_SOURCE_DIR}/docs/logo-256.png")
target_sources(${_app} PRIVATE RenderKit.cpp RenderWindow.cpp Mp3Writer.cpp Source.cpp)
target_compile_definitions(${_app} PRIVATE
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_REPORT_APP_USAGE=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_APPLICATION_NAME_STRING="$<TARGET_PROPERTY:${_app},JUCE_PRODUCT_NAME>"
    JUCE_APPLICATION_VERSION_STRING="$<TARGET_PROPERTY:${_app},JUCE_VERSION>"
    _CRT_SECURE_NO_WARNINGS)
target_link_libraries(${_app}
    PRIVATE
        NoctuaryCore
        juce::juce_gui_basics
        juce::juce_data_structures
        juce::juce_audio_formats
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags)
if(MSVC)
    target_compile_options(${_app} PRIVATE /utf-8)
endif()
if(WIN32)
    target_link_libraries(${_app} PRIVATE mfplat mfreadwrite mfuuid ole32)   # MP3 through Media Foundation
    target_link_options(${_app} PRIVATE "LINKER:/STACK:67108864")   # as ambient_render: the command line renders on main
elseif(APPLE)
    target_link_options(${_app} PRIVATE "LINKER:-stack_size,0x3F00000")
endif()
# Into bin/<preset> after every build (cmake/Family.cmake).
family_bin(${_app})
# Every format written, read back and measured (a few seconds of the first journey each), and the queue.
if(AMBIENT_BUILD_TOOLS)
    # From the source tree: its Library (the packs, the journeys) is found there without an installed copy.
    add_test(NAME renderapp COMMAND ${_app} --selftest 6 WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
    set_tests_properties(renderapp PROPERTIES LABELS "audio" TIMEOUT 900)
endif()
