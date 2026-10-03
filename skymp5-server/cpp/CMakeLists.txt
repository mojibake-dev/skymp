enable_testing()

include(${CMAKE_SOURCE_DIR}/cmake/apply_default_settings.cmake)

#
# MpClientPlugin.dll: the Rust client (thuum ADR-019), skymp-wire's
# wire-client-ffi cdylib with the seven C exports Skyrim Platform calls, built
# by cmake/wire.cmake and copied to where dev_service picks it up.
#

if(WIN32)
  # cmake/wire.cmake is included by skymp5-server/CMakeLists.txt before this
  # directory: the cxx bridge's generated headers are GENERATED only in the
  # directory that includes it, which must be the addon's.
  add_custom_target(MpClientPlugin ALL
    COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>"
    COMMAND ${CMAKE_COMMAND} -E copy "$<TARGET_FILE:skymp_wire-shared>" "${CMAKE_CURRENT_BINARY_DIR}/$<CONFIG>/MpClientPlugin.dll"
    COMMENT "MpClientPlugin.dll from skymp-wire")
  add_dependencies(MpClientPlugin cargo-build_skymp_wire)

  # Not mistake that skyrim-platform depends on MpClientPlugin
  # skyrim-platform is a logical target that invokes dev_service
  # dev_service wants to access built MpClientPlugin.dll
  add_dependencies(skyrim-platform MpClientPlugin)
endif()

# fakeclient: the Rust one (skymp-wire/crates/wire-fakeclient), built and
# placed in dist/server by cmake/wire.cmake (thuum ADR-019).

#
# messages
#

file(GLOB_RECURSE src "${CMAKE_CURRENT_SOURCE_DIR}/messages/*")
list(APPEND src "${CMAKE_SOURCE_DIR}/.clang-format")
add_library(messages STATIC ${src})
target_include_directories(messages PUBLIC "${CMAKE_CURRENT_LIST_DIR}/messages")
target_link_libraries(messages PUBLIC viet)
target_link_libraries(messages PUBLIC papyrus-vm-lib) # Utils.h: stricmp
target_link_libraries(messages PUBLIC serialization)
apply_default_settings(TARGETS messages)
list(APPEND VCPKG_DEPENDENT messages)

#
# mp_common
#

file(GLOB_RECURSE src "${CMAKE_CURRENT_SOURCE_DIR}/mp_common/*")
list(APPEND src "${CMAKE_SOURCE_DIR}/.clang-format")
add_library(mp_common STATIC ${src})
target_compile_definitions(mp_common PUBLIC MAX_PLAYERS=1000)
target_include_directories(mp_common PUBLIC "${CMAKE_CURRENT_LIST_DIR}/mp_common")
target_link_libraries(mp_common PUBLIC messages viet)
apply_default_settings(TARGETS mp_common)
list(APPEND VCPKG_DEPENDENT mp_common)
if(WIN32)
  target_link_libraries(mp_common PUBLIC ws2_32.lib)
endif()

#
# geo
#

# https://www.codeproject.com/Articles/1065730/Point-Inside-Convex-Polygon-in-Cplusplus
file(GLOB_RECURSE src "${CMAKE_CURRENT_SOURCE_DIR}/geo/*")
list(APPEND src "${CMAKE_SOURCE_DIR}/.clang-format")
add_library(geo STATIC ${src})
target_include_directories(geo PUBLIC "${CMAKE_CURRENT_LIST_DIR}/geo")
apply_default_settings(TARGETS geo)

#
# server_guest_lib
#

file(GLOB_RECURSE src "${CMAKE_CURRENT_SOURCE_DIR}/server_guest_lib/*")
list(APPEND src "${CMAKE_SOURCE_DIR}/.clang-format")
add_library(server_guest_lib STATIC ${src})
target_include_directories(server_guest_lib PUBLIC "${CMAKE_CURRENT_LIST_DIR}/server_guest_lib")
apply_default_settings(TARGETS server_guest_lib)
list(APPEND VCPKG_DEPENDENT server_guest_lib)
target_link_libraries(server_guest_lib PUBLIC viet mp_common espm papyrus-vm-lib geo serialization)
# the game rules in Rust (thuum ADR-020); absent only in the emscripten build
if(TARGET wire_bridge_cxx)
  target_link_libraries(server_guest_lib PUBLIC wire_bridge_cxx)
endif()

#
# localization-provider
#

file(GLOB src "${CMAKE_CURRENT_SOURCE_DIR}/localization_provider/*")
list(APPEND src "${CMAKE_SOURCE_DIR}/.clang-format")
add_library(localization-provider STATIC ${src})
apply_default_settings(TARGETS localization-provider)
target_include_directories(localization-provider PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/localization_provider")

#
# Link vcpkg deps
#

find_path(CATCH_INCLUDE_DIR NAMES catch.hpp PATH_SUFFIXES catch2)
get_filename_component(CATCH_INCLUDE_DIR ${CATCH_INCLUDE_DIR} DIRECTORY)

find_path(JSON_INCLUDE_DIR NAMES json.hpp PATH_SUFFIXES nlohmann)
get_filename_component(JSON_INCLUDE_DIR ${JSON_INCLUDE_DIR} DIRECTORY)

find_package(spdlog CONFIG REQUIRED)

find_package(simdjson CONFIG REQUIRED)


find_package(ZLIB REQUIRED)

if(NOT EMSCRIPTEN)
  find_package(mongocxx REQUIRED)
endif()

find_package(unofficial-sodium CONFIG REQUIRED)

# MacOS not supported by rsm-bsa
if(NOT APPLE AND NOT EMSCRIPTEN)
  find_package(bsa CONFIG REQUIRED)
endif()

find_package(libzippp REQUIRED)

find_package(antigo REQUIRED)

foreach(target ${VCPKG_DEPENDENT})
  target_include_directories(${target} PUBLIC ${CATCH_INCLUDE_DIR})
  target_include_directories(${target} PUBLIC ${JSON_INCLUDE_DIR})
  target_link_libraries(${target} PUBLIC spdlog::spdlog)
  target_link_libraries(${target} PUBLIC simdjson::simdjson)


  target_link_libraries(${target} PUBLIC antigo::antigo)

  target_link_libraries(${target} PUBLIC ZLIB::ZLIB)

  if(TARGET mongo::mongocxx_static)
    target_link_libraries(${target} PUBLIC mongo::mongocxx_static mongo::bsoncxx_static mongoc::static bson::static)
  else()
    target_compile_definitions(${target} PUBLIC NO_MONGO=1)
  endif()

  target_link_libraries(${target} PUBLIC unofficial-sodium::sodium)

  if(TARGET bsa::bsa)
    target_link_libraries(${target} PUBLIC bsa::bsa)
  else()
    target_compile_definitions(${target} PUBLIC NO_BSA=1)
  endif()
  target_link_libraries(${target} PUBLIC libzippp::libzippp)

  if(APPLE)
    target_link_libraries(${target} PUBLIC pthread resolv m)
  elseif(WIN32)
    target_link_libraries(${target} PUBLIC Secur32.lib Crypt32.lib Dnsapi.lib Bcrypt.lib)
  elseif(EMSCRIPTEN)
    # ...
  elseif(UNIX)
    target_link_libraries(${target} PUBLIC pthread resolv rt m)
  endif()
endforeach()
