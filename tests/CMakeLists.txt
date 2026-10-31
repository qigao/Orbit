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

  # Two real strict-FR0 Provider DSOs in the normal native graph, linked to
  # the canonical Salts Plugin/ComponentProvider ABI (no loader reimplementation).
  add_library(orbit_dso_provider_n SHARED fixtures/orbit_dso_provider.c)
  add_library(orbit_dso_provider_next SHARED fixtures/orbit_dso_provider.c)
  foreach(provider IN ITEMS orbit_dso_provider_n orbit_dso_provider_next)
    target_compile_features(${provider} PRIVATE c_std_11)
    target_link_libraries(${provider}
      PRIVATE Salts::ComponentProviderABI Salts::PluginABI)
  endforeach()
  target_compile_definitions(orbit_dso_provider_n
    PRIVATE ORBIT_DSO_PROVIDER_INDEX=1)
  target_compile_definitions(orbit_dso_provider_next
    PRIVATE ORBIT_DSO_PROVIDER_INDEX=2)

  add_executable(orbit_scope_dso_native_test test_orbit_scope_dso.c)
  target_compile_features(orbit_scope_dso_native_test PRIVATE c_std_11)
  target_link_libraries(orbit_scope_dso_native_test PRIVATE Orbit::ACE)
  target_compile_definitions(orbit_scope_dso_native_test PRIVATE
    "ORBIT_DSO_PROVIDER_N_PATH=\"$<TARGET_FILE:orbit_dso_provider_n>\""
    "ORBIT_DSO_PROVIDER_NEXT_PATH=\"$<TARGET_FILE:orbit_dso_provider_next>\"")
  add_dependencies(orbit_scope_dso_native_test
    orbit_dso_provider_n orbit_dso_provider_next)
  add_test(NAME orbit.ace.dso_native COMMAND orbit_scope_dso_native_test)
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
