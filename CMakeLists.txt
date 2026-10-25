cmake_minimum_required(VERSION 2.8.12...3.29)
set(TARGET_NAME oracle)
project(${TARGET_NAME})

if(NOT MSVC)
  set(ORACLE_SCANNER_EXTRA_CFLAGS
      "-Wno-pedantic -Wno-sign-compare -Wno-unused-variable")
endif()

set(CMAKE_CXX_FLAGS_DEBUG
    "${CMAKE_CXX_FLAGS_DEBUG} ${ORACLE_SCANNER_EXTRA_CFLAGS}")
set(CMAKE_CXX_FLAGS_RELEASE
    "${CMAKE_CXX_FLAGS_RELEASE} ${ORACLE_SCANNER_EXTRA_CFLAGS}")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO
    "${CMAKE_CXX_FLAGS_RELWITHDEBINFO} ${ORACLE_SCANNER_EXTRA_CFLAGS}")

# Download ODPI-C if not present
if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/odpi)
  message(STATUS "Downloading ODPI-C source code")
  file(
    DOWNLOAD
    "https://github.com/oracle/odpi/archive/refs/tags/v26.0.0.tar.gz"
    ${CMAKE_CURRENT_SOURCE_DIR}/odpi.tar.gz
    SHOW_PROGRESS
    STATUS ODPI_DOWNLOAD_RESULT)

  list(GET ODPI_DOWNLOAD_RESULT 0 ODPI_DOWNLOAD_STATUS)
  if(NOT ODPI_DOWNLOAD_STATUS EQUAL 0)
    file(REMOVE ${CMAKE_CURRENT_SOURCE_DIR}/odpi.tar.gz)
    message(FATAL_ERROR "Failed to download ODPI-C source code")
  endif()

  message(STATUS "Extracting ODPI-C source code")
  file(ARCHIVE_EXTRACT INPUT ${CMAKE_CURRENT_SOURCE_DIR}/odpi.tar.gz
       DESTINATION ${CMAKE_CURRENT_SOURCE_DIR}/odpi_tmp)

  file(GLOB ODPI_EXTRACTED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/odpi_tmp/*")
  file(RENAME ${ODPI_EXTRACTED_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/odpi)
  file(REMOVE_RECURSE ${CMAKE_CURRENT_SOURCE_DIR}/odpi_tmp)
  file(REMOVE ${CMAKE_CURRENT_SOURCE_DIR}/odpi.tar.gz)

  message(STATUS "Finished setting up ODPI-C source code!")
endif()

# Collect ODPI-C source files
file(GLOB ODPI_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/odpi/src/*.c")

function(PREPEND var prefix)
  set(listVar "")
  foreach(f ${ARGN})
    list(APPEND listVar "${prefix}/${f}")
  endforeach(f)
  set(${var}
      "${listVar}"
      PARENT_SCOPE)
endfunction(PREPEND)

include_directories(include ${CMAKE_CURRENT_SOURCE_DIR}/odpi/include)

add_subdirectory(src)

set(PARAMETERS "-no-warnings")
build_loadable_extension(${TARGET_NAME} ${PARAMETERS} ${ALL_OBJECT_FILES}
                         ${ODPI_SOURCES})

target_include_directories(
  ${TARGET_NAME}_loadable_extension
  PRIVATE include ${CMAKE_CURRENT_SOURCE_DIR}/odpi/include)

if(WIN32)
  # On Windows, Oracle Instant Client provides oci.dll which ODPI-C loads at runtime
  # No explicit linking needed - ODPI-C uses dynamic loading
  target_link_libraries(${TARGET_NAME}_loadable_extension)
endif()

set_property(TARGET ${TARGET_NAME}_loadable_extension PROPERTY C_STANDARD 11)
