# kettle-update-helper.exe: installs a downloaded Kettle Edition update after qBittorrent has exited, and undoes it
# if the new version does not run properly. It uses plain Win32 only (no Qt, no other library) and has its C runtime built in,
# so it keeps working when the Qt files it replaces are damaged.
add_executable(kettle_update_helper WIN32 main.cpp)

set_target_properties(kettle_update_helper PROPERTIES
    OUTPUT_NAME kettle-update-helper
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    CXX_STANDARD 20
    CXX_STANDARD_REQUIRED ON
    MSVC_RUNTIME_LIBRARY "MultiThreaded"
)
target_compile_definitions(kettle_update_helper PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(kettle_update_helper PRIVATE bcrypt shell32)
