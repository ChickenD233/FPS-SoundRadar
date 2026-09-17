# engine/CMakeLists.txt - SoundRadar.exe: audio engine + overlay + tray (no third-party deps)

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
    tray.cpp
    simulate.cpp
    gui.cpp
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

if(MSVC)
    # /utf-8: bilingual (Chinese) string literals in tray menu
    target_compile_options(SoundRadar PRIVATE /W4 /O2 /permissive- /utf-8)
    # static CRT: the exe runs on machines without the VC++ redistributable
    set_property(TARGET SoundRadar PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

target_link_libraries(SoundRadar PRIVATE
    ole32
    avrt
    winmm
    comctl32
    d3d11
    dxgi
    d2d1
    dwrite
    dcomp
    windowscodecs
)
