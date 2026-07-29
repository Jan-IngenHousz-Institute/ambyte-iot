cmake_minimum_required(VERSION 3.16.0)

# CI supplies the firmware version through one narrowly named environment
# variable. ESP-IDF reads the CMake PROJECT_VER variable; it does not import an
# arbitrary environment variable for us. Set and validate it before loading the
# ESP-IDF project machinery so the value embedded in esp_app_desc_t is the same
# value the release analysis selected.
set(_ambyte_project_ver "$ENV{AMBYTE_PROJECT_VER}")
if(NOT _ambyte_project_ver STREQUAL "")
    string(LENGTH "${_ambyte_project_ver}" _ambyte_project_ver_length)
    if(_ambyte_project_ver_length GREATER 31)
        message(FATAL_ERROR
            "AMBYTE_PROJECT_VER must fit esp_app_desc_t.version (31 ASCII characters maximum)")
    endif()

    set(_ambyte_release_version_regex
        "^(0|[1-9][0-9]*)[.](0|[1-9][0-9]*)[.](0|[1-9][0-9]*)$")
    set(_ambyte_pr_version_regex "^pr-[1-9][0-9]*-[0-9a-f]+$")
    set(_ambyte_valid_project_ver FALSE)

    if(_ambyte_project_ver MATCHES "${_ambyte_release_version_regex}")
        set(_ambyte_valid_project_ver TRUE)
    elseif(_ambyte_project_ver MATCHES "${_ambyte_pr_version_regex}")
        string(REGEX REPLACE "^pr-[1-9][0-9]*-" "" _ambyte_pr_sha "${_ambyte_project_ver}")
        string(LENGTH "${_ambyte_pr_sha}" _ambyte_pr_sha_length)
        if(_ambyte_pr_sha_length EQUAL 12)
            set(_ambyte_valid_project_ver TRUE)
        endif()
    endif()

    if(NOT _ambyte_valid_project_ver)
        message(FATAL_ERROR
            "Invalid AMBYTE_PROJECT_VER '${_ambyte_project_ver}'. Expected bare semantic version "
            "M.m.p (no leading zeroes) or pr-<positive PR>-<12 lowercase hex>.")
    endif()
    set(PROJECT_VER "${_ambyte_project_ver}")
else()
    # Developer builds remain convenient and self-identifying without restoring
    # a mutable version.txt authority. Remove a conventional leading "v" so the
    # boot/status strings match release versions.
    execute_process(
        COMMAND git -C "${CMAKE_SOURCE_DIR}" describe --tags --always --dirty
        OUTPUT_VARIABLE _ambyte_git_describe
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE _ambyte_git_describe_result
    )
    if(NOT _ambyte_git_describe_result EQUAL 0 OR _ambyte_git_describe STREQUAL "")
        message(FATAL_ERROR
            "AMBYTE_PROJECT_VER is unset and git describe could not determine a local version")
    endif()
    string(REGEX REPLACE "^v" "" PROJECT_VER "${_ambyte_git_describe}")
    string(LENGTH "${PROJECT_VER}" _ambyte_git_version_length)
    if(_ambyte_git_version_length GREATER 31)
        message(FATAL_ERROR
            "git describe version '${PROJECT_VER}' exceeds esp_app_desc_t.version (31 characters)")
    endif()
endif()

list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_SOURCE_DIR}/components/hal")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ambyte-iot)
