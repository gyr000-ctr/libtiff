# Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
# file Copyright.txt or https://cmake.org/licensing for details.

#[=======================================================================[.rst:
FindAVIF
--------

Find the native libavif includes and library.

IMPORTED Targets
^^^^^^^^^^^^^^^^

This module defines :prop_tgt:`IMPORTED` target ``AVIF::avif``, if
libavif has been found.

Result Variables
^^^^^^^^^^^^^^^^

This module defines the following variables:

::

  AVIF_INCLUDE_DIRS   - where to find avif/avif.h, etc.
  AVIF_LIBRARIES      - List of libraries when using libavif.
  AVIF_FOUND          - True if libavif found.
#]=======================================================================]

# Standard names to search
set(AVIF_NAMES avif)
set(AVIF_NAMES_DEBUG avifd)

find_path(AVIF_INCLUDE_DIR
          NAMES avif/avif.h
          PATH_SUFFIXES include)

# Allow AVIF_LIBRARY to be set manually, as the location of the libavif
# library
if(NOT AVIF_LIBRARY)
    find_library(AVIF_LIBRARY_RELEASE
                 NAMES ${AVIF_NAMES}
                 PATH_SUFFIXES lib)
    find_library(AVIF_LIBRARY_DEBUG
                 NAMES ${AVIF_NAMES_DEBUG}
                 PATH_SUFFIXES lib)

    include(SelectLibraryConfigurations)
    select_library_configurations(AVIF)
endif()

unset(AVIF_NAMES)
unset(AVIF_NAMES_DEBUG)

mark_as_advanced(AVIF_INCLUDE_DIR)

include(FindPackageHandleStandardArgs)
FIND_PACKAGE_HANDLE_STANDARD_ARGS(AVIF
        REQUIRED_VARS AVIF_LIBRARY AVIF_INCLUDE_DIR)

if(AVIF_FOUND)
    set(AVIF_INCLUDE_DIRS ${AVIF_INCLUDE_DIR})

    if(NOT AVIF_LIBRARIES)
        set(AVIF_LIBRARIES ${AVIF_LIBRARY})
    endif()

    if(NOT TARGET AVIF::avif)
        add_library(AVIF::avif UNKNOWN IMPORTED)
        set_target_properties(AVIF::avif PROPERTIES
                INTERFACE_INCLUDE_DIRECTORIES "${AVIF_INCLUDE_DIRS}")

        if(AVIF_LIBRARY_RELEASE)
            set_property(TARGET AVIF::avif APPEND PROPERTY
                    IMPORTED_CONFIGURATIONS RELEASE)
            set_target_properties(AVIF::avif PROPERTIES
                    IMPORTED_LOCATION_RELEASE "${AVIF_LIBRARY_RELEASE}")
        endif()

        if(AVIF_LIBRARY_DEBUG)
            set_property(TARGET AVIF::avif APPEND PROPERTY
                    IMPORTED_CONFIGURATIONS DEBUG)
            set_target_properties(AVIF::avif PROPERTIES
                    IMPORTED_LOCATION_DEBUG "${AVIF_LIBRARY_DEBUG}")
        endif()

        if(NOT AVIF_LIBRARY_RELEASE AND NOT AVIF_LIBRARY_DEBUG)
            set_target_properties(AVIF::avif PROPERTIES
                    IMPORTED_LOCATION "${AVIF_LIBRARY}")
        endif()
    endif()
endif()
