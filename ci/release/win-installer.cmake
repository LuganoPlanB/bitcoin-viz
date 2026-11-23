include("${CMAKE_CURRENT_LIST_DIR}/common.cmake")
if(NOT IS_DIRECTORY "${BUILD_DIR}" OR NOT EXISTS "${BUILD_DIR}/cmake_install.cmake")
  message(FATAL_ERROR "BUILD_DIR must point to a configured CMake build")
endif()
file(REAL_PATH "${BUILD_DIR}" build_root)
file(TO_CMAKE_PATH "$ENV{ProgramFiles\(x86\)}" program_files_x86)
find_program(ISCC NAMES ISCC.exe HINTS "${program_files_x86}/Inno Setup 6" REQUIRED)

# Stage only files installed by this build.
string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef nonce)
set(STAGE "${build_root}/installer-stage-${nonce}")
set(OUTPUT "${build_root}/installer")
if(EXISTS "${STAGE}")
  message(FATAL_ERROR "Installer stage already exists: ${STAGE}")
endif()
if(VCPKG_INSTALLED_DIR AND VCPKG_TARGET_TRIPLET)
  set(dependency_arg "-DDEPENDENCIES=${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" --install "${build_root}" --config "${CONFIG}" --prefix "${STAGE}"
  RESULT_VARIABLE install_result)
if(install_result STREQUAL "0")
  execute_process(COMMAND "${CMAKE_COMMAND}"
    "-DVERSION=${VERSION}" -DPLATFORM=windows -DARCH=x86_64 "-DSTAGE=${STAGE}" "-DOUTPUT=${OUTPUT}"
    "-DISCC=${ISCC}" ${dependency_arg} -P "${CMAKE_CURRENT_LIST_DIR}/package.cmake"
    RESULT_VARIABLE package_result)
endif()
file(REMOVE_RECURSE "${STAGE}")
if(NOT install_result STREQUAL "0" OR NOT package_result STREQUAL "0")
  message(FATAL_ERROR "Windows installer build failed")
endif()
