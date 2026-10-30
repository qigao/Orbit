cmake_minimum_required(VERSION 3.25)
project(Orbit VERSION 0.1.0 LANGUAGES C)

include(CTest)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

# Explicit SDK epoch, no retired runtime or opportunistic source fallback.
find_package(Salts 3.0 CONFIG REQUIRED)
if(NOT TARGET Salts::CFlow)
  message(FATAL_ERROR "Orbit::Core requires the installed Salts::CFlow target")
endif()

add_library(orbit_core STATIC core/src/service.c)
add_library(Orbit::Core ALIAS orbit_core)
set_target_properties(orbit_core PROPERTIES EXPORT_NAME Core)
target_compile_features(orbit_core PUBLIC c_std_11)
target_include_directories(orbit_core PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/core/include>
  $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(orbit_core PUBLIC Salts::CFlow)

if(BUILD_TESTING)
  add_subdirectory(tests)
endif()

install(TARGETS orbit_core EXPORT OrbitTargets
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(DIRECTORY core/include/orbit/
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/orbit)
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
