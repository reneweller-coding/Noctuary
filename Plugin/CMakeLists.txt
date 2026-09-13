juce_add_plugin(Noctuary
    COMPANY_NAME                "Rene Weller"
    BUNDLE_ID                   "com.reneweller.noctuary"
    IS_SYNTH                    TRUE
    NEEDS_MIDI_INPUT            TRUE
    NEEDS_MIDI_OUTPUT           FALSE
    IS_MIDI_EFFECT              FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD     FALSE
    PLUGIN_MANUFACTURER_CODE    Rnwl
    PLUGIN_CODE                 Noct
    FORMATS                     VST3 Standalone
    VST3_CATEGORIES             Instrument Synth
    ICON_BIG                    "${CMAKE_CURRENT_SOURCE_DIR}/../docs/logo-256.png"
    ICON_SMALL                  "${CMAKE_CURRENT_SOURCE_DIR}/../docs/logo-64.png"
    VERSION                     ${PROJECT_VERSION}
    PRODUCT_NAME                "Noctuary")

target_sources(Noctuary PRIVATE
    PluginProcessor.cpp
    PluginEditor.cpp
    EditorViews.cpp
    EditorSpectrum.cpp
    EditorMatrix.cpp
    EditorModStrip.cpp
    EditorBrowse.cpp
    EditorDrag.cpp
    NoctuaryLookAndFeel.cpp)

target_compile_definitions(Noctuary PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_VST3_CAN_REPLACE_VST2=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_REPORT_APP_USAGE=0)

target_link_libraries(Noctuary
    PRIVATE
        NoctuaryCore
        juce::juce_audio_utils
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags)
