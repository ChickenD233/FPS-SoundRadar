# engine/CMakeLists.txt - SoundRadar.exe: audio engine + overlay + tray + WebView2 GUI
# (no third-party deps; WebView2 SDK is a system runtime + NuGet headers/loader)

find_program(PYTHON_EXE python REQUIRED)

# embed the HTML UI into a C++ header at configure time
execute_process(
    COMMAND ${PYTHON_EXE} ${CMAKE_CURRENT_SOURCE_DIR}/embed_html.py
            ${CMAKE_CURRENT_SOURCE_DIR}/gui_web.html
            ${CMAKE_CURRENT_BINARY_DIR}/gui_web_html.h
    RESULT_VARIABLE embed_rc
)
if(NOT embed_rc EQUAL 0)
    message(FATAL_ERROR "embed_html.py failed: ${embed_rc}")
endif()

set(WEBVIEW2_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../tools/webview2/build/native)

add_executable(SoundRadar
    main.cpp
    capture.cpp
    render.cpp
    downmix.cpp
    analysis.cpp
    config.cpp
    wasapi_util.cpp
    selftest.cpp
    classify.cpp
    measure.cpp
    pantest.cpp
    devicedefault.cpp
    tray.cpp
    simulate.cpp
    gui2.cpp
    log.cpp
    ../overlay/overlay.cpp
)

target_compile_features(SoundRadar PRIVATE cxx_std_17)
set_target_properties(SoundRadar PROPERTIES CXX_EXTENSIONS OFF)

target_compile_definitions(SoundRadar PRIVATE
    UNICODE
    _UNICODE
    NOMINMAX
    WIN32_LEAN_AND_MEAN
    _CRT_SECURE_NO_WARNINGS
)

target_include_directories(SoundRadar PRIVATE
    ${CMAKE_CURRENT_BINARY_DIR}          # gui_web_html.h
    ${WEBVIEW2_DIR}/include              # WebView2.h
)

if(MSVC)
    # /utf-8: bilingual (Chinese) string literals
    target_compile_options(SoundRadar PRIVATE /W4 /O2 /permissive- /utf-8)
endif()

target_link_libraries(SoundRadar PRIVATE
    ole32
    avrt
    winmm
    comctl32
    shlwapi
    uxtheme
    d3d11
    dxgi
    d2d1
    dwrite
    dcomp
    windowscodecs
    ${WEBVIEW2_DIR}/x64/WebView2LoaderStatic.lib # static loader: no DLL shipping
)
