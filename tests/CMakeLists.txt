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

# DSO acceptance has a single generated Context-v1 implementation; no
# hand-written non-IDL Provider adapter is compiled as an alternative.
option(ORBIT_IDL_CONFORMANCE "Compile and run genuine generated unary Service" OFF)

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

  if(ORBIT_IDL_CONFORMANCE)
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

# M0d: real salts-idlc generated header, built by the existing CMake/CTest
# graph against the actual Salts 3 / Orbit Core SDK. This is opt-in until
# both upstream canonical SDK epochs can be admitted; no probe/consumer
# project or copied/handwritten generated output is allowed.
if(ORBIT_IDL_CONFORMANCE)
  # Only an immutable host CLI is used. A separate Windows tool executable
  # has the same canonical generator semantics and must not be linked to
  # the Salts 3 native runtime.
  set(_orbit_idlc "${ORBIT_IDL_SDK_ROOT}/utils/bin/salts-idlc${CMAKE_EXECUTABLE_SUFFIX}")
  if(NOT DEFINED ORBIT_IDL_SDK_ROOT OR
     NOT EXISTS "${_orbit_idlc}" OR
     NOT EXISTS "${ORBIT_IDL_SDK_ROOT}/utils/include/data_bind_binding_plan.h")
    message(FATAL_ERROR
      "ORBIT_IDL_CONFORMANCE requires the exact installed salts-idlc + headers")
  endif()
  if(WIN32)
    set(_orbit_idl_env
      --modify "PATH=path_list_prepend:${ORBIT_IDL_SDK_ROOT}/salts/bin"
      --modify "PATH=path_list_prepend:${ORBIT_IDL_SDK_ROOT}/utils/bin")
  else()
    set(_orbit_idl_env
      --modify "LD_LIBRARY_PATH=path_list_prepend:${ORBIT_IDL_SDK_ROOT}/salts/lib")
  endif()
  set(_orbit_idl_dir "${CMAKE_CURRENT_BINARY_DIR}/generated-idl")
  set(_orbit_idl_records "${_orbit_idl_dir}/game_records.h")
  set(_orbit_idl_header "${_orbit_idl_dir}/game.service_native.h")
  set(_orbit_idl_source "${_orbit_idl_dir}/game.service_native.c")
  add_custom_command(
    OUTPUT "${_orbit_idl_records}" "${_orbit_idl_header}" "${_orbit_idl_source}"
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${_orbit_idl_dir}"
    COMMAND "${CMAKE_COMMAND}" -E env ${_orbit_idl_env}
      "${_orbit_idlc}"
      "${CMAKE_CURRENT_SOURCE_DIR}/idl/game.schema" --lang c
      --output "${_orbit_idl_records}"
      --artifacts native --artifact-name game
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/idl/game.schema"
            "${_orbit_idlc}"
    VERBATIM
    COMMENT "Generate Orbit unary Client/Dispatcher from canonical salts-idlc")
  add_executable(orbit_generated_idl_native_test
    test_orbit_generated_idl.c test_orbit_generated_cpp17.cpp
    test_orbit_multi_room.c
    "${_orbit_idl_header}" "${_orbit_idl_records}")
  target_compile_features(orbit_generated_idl_native_test PRIVATE
    c_std_11 cxx_std_17)
  # Host SDK serves only the external generator PROCESS. Never admit its
  # Salts2.3 headers into the Salts3 Orbit runtime compilation.
  target_include_directories(orbit_generated_idl_native_test PRIVATE
    "${_orbit_idl_dir}")
  target_link_libraries(orbit_generated_idl_native_test
    PRIVATE Orbit::Core Salts::Platform)
  add_test(NAME orbit.idl.generated_unary
    COMMAND orbit_generated_idl_native_test)

  # The canonical DSO/ACE native test reuses the ONE generated Contract.
  # With IDL conformance OFF, this test target is not created; there is
  # no stale hand-written Provider dispatch fallback.
  if(ORBIT_BUILD_ACE)
    target_sources(orbit_scope_dso_native_test PRIVATE
      "${_orbit_idl_header}" "${_orbit_idl_records}")
    target_include_directories(orbit_scope_dso_native_test PRIVATE
      "${_orbit_idl_dir}")
  endif()
endif()
