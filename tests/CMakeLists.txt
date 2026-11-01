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
# Same native test executable, one additional negative owner-lifetime mode:
# a malformed no-fail Provider must never free an incompletely cleaned VALUE.
# No new CTest executable/project or CI workflow.
add_test(NAME orbit.core.cleanup_quarantine
  COMMAND orbit_call_native_test --teardown-quarantine)

# DSO acceptance has a single generated Context-v1 implementation; no
# hand-written non-IDL Provider adapter is compiled as an alternative.
option(ORBIT_IDL_CONFORMANCE "Compile and run genuine generated unary Service" OFF)

if(ORBIT_BUILD_ACE)
  add_executable(orbit_scope_native_test test_orbit_scope.c)
  target_compile_features(orbit_scope_native_test PRIVATE c_std_11)
  target_link_libraries(orbit_scope_native_test PRIVATE Orbit::ACE)
  add_test(NAME orbit.ace.scope_native COMMAND orbit_scope_native_test)

  # Actual static Configurator Provider + ServiceHost N -> N+1 + CFlow Actor/Call.
  add_executable(orbit_scope_provider_native_test test_orbit_scope_provider.c)
  target_compile_features(orbit_scope_provider_native_test PRIVATE c_std_11)
  target_link_libraries(orbit_scope_provider_native_test PRIVATE Orbit::ACE)
  add_test(NAME orbit.ace.provider_native COMMAND orbit_scope_provider_native_test)

  if(ORBIT_IDL_CONFORMANCE)
    # Two real strict-FR0 Provider DSOs in the normal native graph, linked to
    # the canonical Salts Plugin/ServiceHost Provider ABI (no loader reimplementation).
    add_library(orbit_dso_provider_n SHARED fixtures/orbit_dso_provider.c)
    add_library(orbit_dso_provider_next SHARED fixtures/orbit_dso_provider.c)
    foreach(provider IN ITEMS orbit_dso_provider_n orbit_dso_provider_next)
      target_compile_features(${provider} PRIVATE c_std_11)
      target_link_libraries(${provider}
        PRIVATE CMeta::Provider)
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
    # Same exact native DSO test executable; isolated process MUST retain
    # a quarantined old Provider/Scope when final CMeta temp cleanup fails.
    # This is an owner-lifetime negative mode, not another SDK or loader.
    add_test(NAME orbit.ace.dso_cleanup_quarantine
      COMMAND orbit_scope_dso_native_test --dso-cleanup-quarantine)
    # A THIRD invocation of that very same native test executable. Here
    # the faulty owning DataDesc and its BufferOps callbacks live INSIDE
    # the original N Provider DSO, not in a host-side test fixture.
    # Faulty owners intentionally remain quarantined at process exit.
    add_test(NAME orbit.ace.dso_authored_cleanup_quarantine
      COMMAND orbit_scope_dso_native_test --dso-authored-cleanup-quarantine)
    # M0k.2o: deterministic two-native-thread schedule in the SAME real
    # DSO CTest executable. Hold the N+1 generated terminal callback while
    # the original N DSO-authored faulty CMeta teardown is quarantined.
    add_test(NAME orbit.ace.dso_authored_callback_overlap
      COMMAND orbit_scope_dso_native_test --dso-authored-callback-overlap)
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
# graph against the actual Salts 4 / Orbit Core SDK. This is opt-in until
# both upstream canonical SDK epochs can be admitted; no probe/consumer
# project or copied/handwritten generated output is allowed.
if(ORBIT_IDL_CONFORMANCE)
  # Only an immutable host CLI is used. A separate Windows tool executable
  # has the same canonical generator semantics and must not be linked to
  # the Salts 4 native runtime.
  set(_orbit_idlc "${ORBIT_IDL_SDK_ROOT}/utils/bin/salts-idlc${CMAKE_EXECUTABLE_SUFFIX}")
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
  # A SECOND schema generated by the SAME canonical salts-idlc host and
  # existing single owner target. This does not create a CTest or CI project.
  set(_orbit_owned_records "${_orbit_idl_dir}/owned_room_records.h")
  set(_orbit_owned_native "${_orbit_idl_dir}/owned_room_native.h")
  set(_orbit_owned_header "${_orbit_idl_dir}/owned_room.service_native.h")
  set(_orbit_owned_source "${_orbit_idl_dir}/owned_room.service_native.c")
  add_custom_command(
    OUTPUT "${_orbit_owned_records}" "${_orbit_owned_native}"
           "${_orbit_owned_header}" "${_orbit_owned_source}"
    COMMAND "${CMAKE_COMMAND}" -E env ${_orbit_idl_env}
      "${_orbit_idlc}"
      "${CMAKE_CURRENT_SOURCE_DIR}/idl/owned_room.schema" --lang c
      --output "${_orbit_owned_records}"
      --artifacts native --artifact-name owned_room
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/idl/owned_room.schema"
            "${_orbit_idlc}"
    VERBATIM
    COMMENT "Generate budgeted owning VALUE Room using canonical salts-idlc")
  # One build owner invokes the compiler. Listing the same generated OUTPUT
  # in two executable projects duplicates the custom rule under MSBuild, which
  # can race the compiler's output backup/publication transaction.
  add_custom_target(orbit_idl_codegen
    DEPENDS "${_orbit_idl_records}" "${_orbit_idl_header}" "${_orbit_idl_source}"
            "${_orbit_owned_records}" "${_orbit_owned_native}"
            "${_orbit_owned_header}" "${_orbit_owned_source}")
  add_executable(orbit_generated_idl_native_test
    test_orbit_generated_idl.c test_orbit_generated_cpp17.cpp
    test_orbit_multi_room.c test_orbit_generated_owned.c)
  add_dependencies(orbit_generated_idl_native_test orbit_idl_codegen)
  target_compile_features(orbit_generated_idl_native_test PRIVATE
    c_std_11 cxx_std_17)
  # Host SDK serves only the external generator PROCESS. Never admit its
  # Salts2.3 headers into the Salts4 Orbit runtime compilation.
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
    add_dependencies(orbit_scope_dso_native_test orbit_idl_codegen)
    target_include_directories(orbit_scope_dso_native_test PRIVATE
      "${_orbit_idl_dir}")
  endif()
endif()
