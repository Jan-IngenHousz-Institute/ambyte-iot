cmake_minimum_required(VERSION 3.16.0)

# CI supplies the semantic-release preview version. When it is unset, local
# builds retain ESP-IDF's git-derived version instead of a second manual source.
if(DEFINED ENV{AMBYTE_PROJECT_VER} AND NOT "$ENV{AMBYTE_PROJECT_VER}" STREQUAL "")
    set(PROJECT_VER "$ENV{AMBYTE_PROJECT_VER}")
endif()

list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_SOURCE_DIR}/components/hal")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ambyte-iot)
