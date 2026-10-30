add_executable(orbit_core_native_test test_orbit_service.c)
target_compile_features(orbit_core_native_test PRIVATE c_std_11)
target_link_libraries(orbit_core_native_test
  PRIVATE Orbit::Core Salts::Platform)
add_test(NAME orbit.core.native COMMAND orbit_core_native_test)
