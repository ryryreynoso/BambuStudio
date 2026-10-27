get_filename_component(_TEST_NAME ${CMAKE_CURRENT_LIST_DIR} NAME)

add_executable(${_TEST_NAME}_tests 
	${_TEST_NAME}_tests.cpp
	test_3mf.cpp
	test_aabbindirect.cpp
	test_clipper_offset.cpp
	test_clipper_utils.cpp
	test_config.cpp
	test_elephant_foot_compensation.cpp
	test_geometry.cpp
	test_placeholder_parser.cpp
	test_polygon.cpp
	test_mutable_polygon.cpp
	test_mutable_priority_queue.cpp
	test_stl.cpp
	test_meshboolean.cpp
	test_marchingsquares.cpp
	test_timeutils.cpp
	test_voronoi.cpp
    test_optimizers.cpp
    test_png_io.cpp
    test_timeutils.cpp
    test_indexed_triangle_set.cpp
    test_triangle_selector.cpp
    test_debounce.cpp
    test_emboss.cpp
    ../libnest2d/printer_parts.cpp
	)

if (TARGET OpenVDB::openvdb)
    target_sources(${_TEST_NAME}_tests PRIVATE test_hollowing.cpp)
endif()

target_link_libraries(${_TEST_NAME}_tests test_common libslic3r)
set_property(TARGET ${_TEST_NAME}_tests PROPERTY FOLDER "tests")

if (WIN32)
    # bambustudio_copy_dlls now requires 4 args (target, config, postfix,
    # output_dlls); legacy 1-arg call sites in tests/* predate that signature
    # change.  Test executables link libslic3r and do not need the heavyweight
    # OCCT / GMP / MPFR / FFmpeg DLL bundle, so skip the copy step here.
    # Run tests with PATH pointing at deps build dir if extra DLLs are needed.
    # bambustudio_copy_dlls(${_TEST_NAME}_tests <config> <postfix> <out>)
endif()

# catch_discover_tests(${_TEST_NAME}_tests TEST_PREFIX "${_TEST_NAME}: ")
add_test(${_TEST_NAME}_tests ${_TEST_NAME}_tests ${CATCH_EXTRA_ARGS})

# Full Spectrum color mixing tests. A separate target so they build and run independently of the
# upstream libslic3r_tests sources (test_3mf.cpp currently fails to compile upstream).
add_executable(full_spectrum_tests
    ${_TEST_NAME}_tests.cpp
    test_filament_mixer_pattern.cpp
    test_filament_mixer_sublayer.cpp
    test_full_spectrum_core.cpp
    test_full_spectrum_import.cpp
    test_full_spectrum_gradient.cpp
    test_moonraker_host.cpp
    test_snapmaker_connect.cpp
    )
# libslic3r references symbols defined in libslic3r_gui (Http, BBL_Encrypt, nanosvg); link it too.
target_link_libraries(full_spectrum_tests test_common libslic3r libslic3r_gui)
# test_moonraker_host.cpp includes the print host headers, which need wxWidgets; reuse the include
# directories and definitions src/ set up for libslic3r_gui.
get_target_property(_slic3r_gui_includes libslic3r_gui INCLUDE_DIRECTORIES)
get_directory_property(_slic3r_gui_definitions DIRECTORY ${CMAKE_SOURCE_DIR}/src COMPILE_DEFINITIONS)
target_include_directories(full_spectrum_tests PRIVATE ${_slic3r_gui_includes})
target_compile_definitions(full_spectrum_tests PRIVATE ${_slic3r_gui_definitions})
set_property(TARGET full_spectrum_tests PROPERTY FOLDER "tests")
add_test(full_spectrum_tests full_spectrum_tests ${CATCH_EXTRA_ARGS})
