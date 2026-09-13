# Noctuary for Meta Quest -- native OpenXR app (NativeActivity, GLES 3, Oboe).
# Configure with the NDK toolchain, see build_apk.ps1.
cmake_minimum_required(VERSION 3.22)
project(NoctuaryQuest LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(AMBIENT_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/..")
set(THIRD "${AMBIENT_ROOT}/ThirdParty")

# The synthesizer core, unchanged.
add_subdirectory("${AMBIENT_ROOT}/Core" Core)

# Khronos OpenXR loader for Android (prefab layout from the Maven AAR, see fetch_thirdparty.ps1).
set(OXR "${THIRD}/openxr-loader/prefab/modules")
add_library(openxr_loader SHARED IMPORTED)
set_target_properties(openxr_loader PROPERTIES
    IMPORTED_LOCATION "${OXR}/openxr_loader/libs/android.${ANDROID_ABI}/libopenxr_loader.so"
    INTERFACE_INCLUDE_DIRECTORIES "${OXR}/headers/include")

# Oboe (low-latency Android audio), built from source as a static library.
set(OBOE_DIR "${THIRD}/oboe")
add_subdirectory("${OBOE_DIR}" oboe)

# NDK native app glue.
add_library(native_app_glue STATIC "${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c")
target_include_directories(native_app_glue PUBLIC "${ANDROID_NDK}/sources/android/native_app_glue")

add_library(ambientquest SHARED src/main.cpp)
target_compile_definitions(ambientquest PRIVATE XR_USE_PLATFORM_ANDROID XR_USE_GRAPHICS_API_OPENGL_ES)
target_link_libraries(ambientquest PRIVATE NoctuaryCore openxr_loader oboe native_app_glue android log EGL GLESv3)
# Keep ANativeActivity_onCreate from the glue library.
target_link_options(ambientquest PRIVATE -u ANativeActivity_onCreate)
if(NOT MSVC)
    target_compile_options(ambientquest PRIVATE -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers)
endif()
