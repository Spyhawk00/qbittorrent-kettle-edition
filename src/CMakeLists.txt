include_directories(${CMAKE_CURRENT_SOURCE_DIR})

add_subdirectory(app)
add_subdirectory(base)

if (GUI)
    add_subdirectory(gui)
    if (WIN32)
        add_subdirectory(updatehelper)
    endif()
endif()

if (WEBUI)
    add_subdirectory(webui)
endif()
