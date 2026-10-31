cmake_minimum_required(VERSION 3.25)
project(Orbit VERSION 0.1.0 LANGUAGES C)

include(CTest)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# Explicit SDK epoch, no retired runtime or opportunistic source fallback.
find_package(Salts 4.0 CONFIG REQUIRED)

add_library(orbit_core STATIC
  core/src/service.c
  core/src/call.c)
add_library(Orbit::Core ALIAS orbit_core)
set_target_properties(orbit_core PROPERTIES EXPORT_NAME Core)
target_compile_features(orbit_core PUBLIC c_std_11)
target_include_directories(orbit_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/core/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(orbit_core
  PUBLIC Salts::CFlow
  PRIVATE Salts::Platform Salts::CMeta)

# Build the optional ACE adapter ONLY when explicitly requested. This is
# a feature selection, not a find/probe/fallback against another SDK epoch.
# A standalone Orbit::Core build must not require Salts::ServiceHost.
option(ORBIT_BUILD_ACE "Build canonical Salts ServiceHost scope adapter" OFF)
if(ORBIT_BUILD_ACE)
  add_library(orbit_ace STATIC ace/src/scope.c)
  add_library(Orbit::ACE ALIAS orbit_ace)
  set_target_properties(orbit_ace PROPERTIES EXPORT_NAME ACE)
  target_compile_features(orbit_ace PUBLIC c_std_11)
  target_include_directories(orbit_ace PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/ace/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
  target_link_libraries(orbit_ace PUBLIC Orbit::Core Salts::ServiceHost)
endif()

if(BUILD_TESTING)
  # C++17 header/ABI acceptance lives in the EXISTING native CTest graph.
  # The shipped Orbit libraries remain pure C11 and require no C++ runtime.
  enable_language(CXX)
  add_subdirectory(tests)
endif()

install(TARGETS orbit_core EXPORT OrbitTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY core/include/orbit/
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/orbit)
if(ORBIT_BUILD_ACE)
  install(TARGETS orbit_ace EXPORT OrbitTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
  install(DIRECTORY ace/include/orbit/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/orbit)
endif()
install(EXPORT OrbitTargets FILE OrbitTargets.cmake NAMESPACE Orbit::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Orbit)

configure_package_config_file(
  "${CMAKE_CURRENT_SOURCE_DIR}/cmake/OrbitConfig.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/OrbitConfig.cmake"
  INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/Orbit")
write_basic_package_version_file(
  "${CMAKE_CURRENT_BINARY_DIR}/OrbitConfigVersion.cmake"
  VERSION "${PROJECT_VERSION}" COMPATIBILITY SameMinorVersion)
install(FILES
  "${CMAKE_CURRENT_BINARY_DIR}/OrbitConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/OrbitConfigVersion.cmake"
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/Orbit")
