add_executable(ambient_render main.cpp)
target_link_libraries(ambient_render PRIVATE NoctuaryCore)
if(MSVC)
    target_compile_options(ambient_render PRIVATE /W4 /permissive- /utf-8)
endif()
# How much stack the program needs is a property of the program, not of the compiler that built
# it: an Engine is a few hundred kilobytes and more than one of them stands on the stack here.
# Tied to MSVC, the option quietly went missing under any other Windows compiler -- Intel's, for
# one -- and the binary died on startup with nothing but an exit code.
if(WIN32)
    target_link_options(ambient_render PRIVATE "LINKER:/STACK:67108864")
endif()

# The conductor's hour, measured without audio: the rule book's section 11 as a tool.
add_executable(ambient_brain_audit brain_audit.cpp)
target_link_libraries(ambient_brain_audit PRIVATE NoctuaryCore)
if(MSVC)
    target_compile_options(ambient_brain_audit PRIVATE /W4 /permissive- /utf-8)
endif()
if(WIN32)
    target_link_options(ambient_brain_audit PRIVATE "LINKER:/STACK:67108864")
endif()
