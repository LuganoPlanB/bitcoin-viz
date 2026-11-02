cmake_minimum_required(VERSION 3.20)
project(bitcoin-wizard-cave VERSION 0.2.0 LANGUAGES CXX)

include(CTest)
include(GNUInstallDirs)
option(BWC_SANITIZE "Enable address and undefined-behavior sanitizers" OFF)
find_program(BWC_CCACHE ccache)
if(BWC_CCACHE AND NOT DEFINED CMAKE_CXX_COMPILER_LAUNCHER)
  set(CMAKE_CXX_COMPILER_LAUNCHER "${BWC_CCACHE}" CACHE STRING "Compiler launcher")
endif()
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
find_package(Threads REQUIRED)
find_package(SQLite3 3.24 REQUIRED)
find_package(CURL 7.68 REQUIRED)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(PkgConfig REQUIRED)
pkg_check_modules(ZMQ REQUIRED IMPORTED_TARGET libzmq>=4.3)

add_library(wizard_native STATIC
  native/src/storage.cpp native/src/model.cpp native/src/core.cpp
  native/src/observer.cpp native/src/render.cpp native/src/capture.cpp)
target_compile_features(wizard_native PUBLIC cxx_std_17)
set_target_properties(wizard_native PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(wizard_native PUBLIC native/include)
target_include_directories(wizard_native SYSTEM PUBLIC native/third_party)
target_link_libraries(wizard_native PUBLIC SQLite::SQLite3 CURL::libcurl
  OpenSSL::Crypto PkgConfig::ZMQ Threads::Threads)
target_compile_options(wizard_native PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic;-Wconversion;-Wshadow>)

add_executable(bitcoin-wizard-cave-native native/src/main.cpp)
target_link_libraries(bitcoin-wizard-cave-native PRIVATE wizard_native)
target_compile_options(bitcoin-wizard-cave-native PRIVATE
  $<$<CXX_COMPILER_ID:GNU,Clang>:-Wall;-Wextra;-Wpedantic>)
if(BWC_SANITIZE)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    message(FATAL_ERROR "BWC_SANITIZE requires GCC or Clang")
  endif()
  target_compile_options(wizard_native PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(wizard_native PUBLIC -fsanitize=address,undefined)
endif()
install(TARGETS bitcoin-wizard-cave-native RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/bitcoin-wizard-cave)
install(FILES native/third_party/nlohmann/LICENSE.MIT
  DESTINATION ${CMAKE_INSTALL_DATADIR}/bitcoin-wizard-cave RENAME nlohmann-LICENSE.MIT)

if(BUILD_TESTING)
  add_executable(wizard-native-contract native/tests/contract.cpp)
  target_link_libraries(wizard-native-contract PRIVATE wizard_native)
  add_test(NAME native-contract COMMAND wizard-native-contract)
  set_tests_properties(native-contract PROPERTIES TIMEOUT 30)
  find_package(Python3 COMPONENTS Interpreter)
  if(Python3_Interpreter_FOUND)
    add_test(NAME python-contract COMMAND ${Python3_EXECUTABLE} -m unittest discover -s tests -q)
    set_tests_properties(python-contract PROPERTIES WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
      ENVIRONMENT "PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}/src" TIMEOUT 30)
    add_test(NAME cross-language-parity COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/native/tests/parity.py $<TARGET_FILE:bitcoin-wizard-cave-native>)
    set_tests_properties(cross-language-parity PROPERTIES TIMEOUT 60)
    add_test(NAME native-observer-integration COMMAND ${Python3_EXECUTABLE}
      ${CMAKE_CURRENT_SOURCE_DIR}/native/tests/observer_integration.py $<TARGET_FILE:bitcoin-wizard-cave-native>)
    set_tests_properties(native-observer-integration PROPERTIES TIMEOUT 40)
  endif()
endif()
