# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

#[=======================================================================[.rst:
FindOpenJPH
-----------

Find the OpenJPH High Throughput JPEG 2000 (HTJ2K) library.

Imported Targets
^^^^^^^^^^^^^^^^

This module defines the following :prop_tgt:`IMPORTED` target:

``OpenJPH::OpenJPH``
  The OpenJPH library, if found.

Result Variables
^^^^^^^^^^^^^^^^

This module defines the following variables:

``OpenJPH_INCLUDE_DIRS``
  where to find openjph/ojph_codestream.h, etc.
``OpenJPH_LIBRARIES``
  List of libraries when using OpenJPH.
``OpenJPH_FOUND``
  True if OpenJPH found.
``OpenJPH_VERSION_STRING``
  The version of the OpenJPH library found.

#]=======================================================================]

find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_check_modules(PC_OPENJPH QUIET openjph)
endif()

find_path(OpenJPH_INCLUDE_DIR
  NAMES openjph/ojph_codestream.h
  HINTS ${PC_OPENJPH_INCLUDE_DIRS})

find_library(OpenJPH_LIBRARY
  NAMES openjph
  HINTS ${PC_OPENJPH_LIBRARY_DIRS})

if(OpenJPH_INCLUDE_DIR AND EXISTS "${OpenJPH_INCLUDE_DIR}/openjph/ojph_version.h")
  file(STRINGS "${OpenJPH_INCLUDE_DIR}/openjph/ojph_version.h" _openjph_ver
       REGEX "^#define[ \t]+OPENJPH_VERSION_(MAJOR|MINOR|PATCH)[ \t]+[0-9]+")
  string(REGEX REPLACE ".*MAJOR[ \t]+([0-9]+).*" "\\1"
         OpenJPH_VERSION_MAJOR "${_openjph_ver}")
  string(REGEX REPLACE ".*MINOR[ \t]+([0-9]+).*" "\\1"
         OpenJPH_VERSION_MINOR "${_openjph_ver}")
  string(REGEX REPLACE ".*PATCH[ \t]+([0-9]+).*" "\\1"
         OpenJPH_VERSION_PATCH "${_openjph_ver}")
  set(OpenJPH_VERSION_STRING
      "${OpenJPH_VERSION_MAJOR}.${OpenJPH_VERSION_MINOR}.${OpenJPH_VERSION_PATCH}")
  unset(_openjph_ver)
endif()

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(OpenJPH
                                  REQUIRED_VARS OpenJPH_LIBRARY
                                                OpenJPH_INCLUDE_DIR
                                  VERSION_VAR OpenJPH_VERSION_STRING)

if(OpenJPH_FOUND)
  set(OpenJPH_LIBRARIES ${OpenJPH_LIBRARY})
  set(OpenJPH_INCLUDE_DIRS ${OpenJPH_INCLUDE_DIR})

  if(NOT TARGET OpenJPH::OpenJPH)
    add_library(OpenJPH::OpenJPH UNKNOWN IMPORTED)
    set_target_properties(OpenJPH::OpenJPH PROPERTIES
      INTERFACE_INCLUDE_DIRECTORIES "${OpenJPH_INCLUDE_DIR}"
      IMPORTED_LINK_INTERFACE_LANGUAGES "CXX"
      IMPORTED_LOCATION "${OpenJPH_LIBRARY}")
  endif()
endif()

mark_as_advanced(OpenJPH_INCLUDE_DIR OpenJPH_LIBRARY)
