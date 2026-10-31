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

if(ORBIT_BUILD_ACE)
  add_executable(orbit_scope_native_test test_orbit_scope.c)
  target_compile_features(orbit_scope_native_test PRIVATE c_std_11)
  target_link_libraries(orbit_scope_native_test PRIVATE Orbit::ACE)
  add_test(NAME orbit.ace.scope_native COMMAND orbit_scope_native_test)

  # Actual static ComponentProvider + ServiceHost N -> N+1 + CFlow Actor/Call.
  add_executable(orbit_scope_provider_native_test test_orbit_scope_provider.c)
  target_compile_features(orbit_scope_provider_native_test PRIVATE c_std_11)
  target_link_libraries(orbit_scope_provider_native_test PRIVATE Orbit::ACE)
  add_test(NAME orbit.ace.provider_native COMMAND orbit_scope_provider_native_test)
endif()

# The normal native test graph owns public C++17 C ABI admission. This is
# not an installer, SDK probe or a second consumer project.
add_executable(orbit_public_cpp_test test_orbit_public_cpp.cpp)
target_compile_features(orbit_public_cpp_test PRIVATE cxx_std_17)
target_link_libraries(orbit_public_cpp_test PRIVATE Orbit::Core)
if(ORBIT_BUILD_ACE)
  target_compile_definitions(orbit_public_cpp_test PRIVATE ORBIT_TEST_HAS_ACE=1)
  target_link_libraries(orbit_public_cpp_test PRIVATE Orbit::ACE)
endif()
add_test(NAME orbit.public.cpp17 COMMAND orbit_public_cpp_test)
