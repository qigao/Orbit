add_executable(orbit_core_native_test test_orbit_service.c)
target_compile_features(orbit_core_native_test PRIVATE c_std_11)
target_link_libraries(orbit_core_native_test
  PRIVATE Orbit::Core Salts::Platform)
add_test(NAME orbit.core.native COMMAND orbit_core_native_test)

add_executable(orbit_call_native_test test_orbit_call.c)
target_compile_features(orbit_call_native_test PRIVATE c_std_11)
target_link_libraries(orbit_call_native_test
  PRIVATE Orbit::Core Salts::Platform)
add_test(NAME orbit.core.call_native COMMAND orbit_call_native_test)

add_executable(orbit_scope_native_test test_orbit_scope.c)
target_compile_features(orbit_scope_native_test PRIVATE c_std_11)
target_link_libraries(orbit_scope_native_test PRIVATE Orbit::ACE)
add_test(NAME orbit.ace.scope_native COMMAND orbit_scope_native_test)
