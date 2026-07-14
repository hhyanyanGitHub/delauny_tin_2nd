cmake_minimum_required(VERSION 3.24)

project(dterrain
    VERSION 0.2.0
    DESCRIPTION "Dynamic 2.5D Delaunay terrain triangulation DLL"
    LANGUAGES CXX)

if(POLICY CMP0167)
    cmake_policy(SET CMP0167 OLD)
endif()

include(GNUInstallDirs)

option(DT_BUILD_TESTS "Build automated tests" ON)
option(DT_BUILD_EXAMPLES "Build example programs" ON)
option(DT_BUILD_BENCHMARKS "Build benchmark programs" OFF)
option(DT_BUILD_GUI "Build native Windows GUI demonstration" ON)

find_package(CGAL CONFIG REQUIRED)

add_library(dterrain SHARED
    src/dt_api.cpp
    src/dt_core.cpp
    src/dt_legacy.cpp)

target_compile_features(dterrain PUBLIC cxx_std_17)
target_compile_definitions(dterrain PRIVATE DT_BUILD_DLL)
target_include_directories(dterrain
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(dterrain PRIVATE CGAL::CGAL)

if(MSVC)
    target_compile_options(dterrain PRIVATE /W4 /permissive- /EHsc)
else()
    target_compile_options(dterrain PRIVATE -Wall -Wextra -Wpedantic)
    if(MINGW)
        # GCC 14 MinGW otherwise emits an incompatible __cxa_thread_atexit
        # declaration for CGAL's static thread-local scratch vectors.
        target_compile_definitions(dterrain PRIVATE CGAL_HAS_NO_THREADS)
        target_link_options(dterrain PRIVATE -static-libgcc -static-libstdc++)
    endif()
endif()

if(MINGW)
    get_filename_component(DT_MINGW_BIN_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
    find_file(DT_WINPTHREAD_DLL libwinpthread-1.dll PATHS "${DT_MINGW_BIN_DIR}"
              NO_DEFAULT_PATH)
    if(DT_WINPTHREAD_DLL)
        add_custom_command(TARGET dterrain POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${DT_WINPTHREAD_DLL}" "$<TARGET_FILE_DIR:dterrain>"
            VERBATIM)
        install(FILES "${DT_WINPTHREAD_DLL}" DESTINATION ${CMAKE_INSTALL_BINDIR})
    endif()
    if(DEFINED VCPKG_INSTALLED_DIR AND DEFINED VCPKG_TARGET_TRIPLET)
        find_file(DT_GMP_RUNTIME_DLL
            NAMES libgmp-10.dll libgmp.dll
            PATHS "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/bin"
            NO_DEFAULT_PATH)
        if(DT_GMP_RUNTIME_DLL)
            add_custom_command(TARGET dterrain POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${DT_GMP_RUNTIME_DLL}" "$<TARGET_FILE_DIR:dterrain>"
                VERBATIM)
            install(FILES "${DT_GMP_RUNTIME_DLL}" DESTINATION ${CMAKE_INSTALL_BINDIR})
        endif()
    endif()
endif()

set_target_properties(dterrain PROPERTIES
    OUTPUT_NAME "dterrain"
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES)

if(WIN32)
    set_target_properties(dterrain PROPERTIES PREFIX "")
endif()

if(DT_BUILD_TESTS)
    enable_testing()
    add_executable(dterrain_tests tests/test_dt.cpp)
    target_compile_features(dterrain_tests PRIVATE cxx_std_17)
    target_link_libraries(dterrain_tests PRIVATE dterrain)
    add_test(NAME dterrain_tests COMMAND dterrain_tests)
endif()

if(DT_BUILD_EXAMPLES)
    add_executable(dterrain_console examples/console_demo.cpp)
    target_compile_features(dterrain_console PRIVATE cxx_std_17)
    target_link_libraries(dterrain_console PRIVATE dterrain)
endif()

if(DT_BUILD_BENCHMARKS)
    add_executable(dterrain_benchmark benchmarks/build_benchmark.cpp)
    target_compile_features(dterrain_benchmark PRIVATE cxx_std_17)
    target_link_libraries(dterrain_benchmark PRIVATE dterrain)
endif()

if(DT_BUILD_GUI AND WIN32)
    add_executable(dterrain_demo WIN32 gui/terrain_demo.cpp)
    target_compile_features(dterrain_demo PRIVATE cxx_std_17)
    target_compile_definitions(dterrain_demo PRIVATE UNICODE _UNICODE NOMINMAX)
    target_link_libraries(dterrain_demo PRIVATE dterrain comdlg32 gdi32 user32)
    if(MINGW)
        target_link_options(dterrain_demo PRIVATE -municode)
        target_link_options(dterrain_demo PRIVATE -static-libgcc -static-libstdc++)
    endif()
endif()

install(TARGETS dterrain
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES include/dt_api.h include/dt_legacy.hpp
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES README.md DESTINATION .)
install(DIRECTORY docs DESTINATION .)
install(FILES examples/sample_points.xyz DESTINATION sample_data)
if(TARGET dterrain_demo)
    install(TARGETS dterrain_demo RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()
