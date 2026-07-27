if (NOT DEFINED AWS_C_IOT_SOURCE_DIR OR NOT IS_DIRECTORY "${AWS_C_IOT_SOURCE_DIR}")
    message(FATAL_ERROR "AWS_C_IOT_SOURCE_DIR must identify an aws-c-iot source directory")
endif ()

if (NOT DEFINED AWS_C_IOT_PATCH_FILE OR NOT EXISTS "${AWS_C_IOT_PATCH_FILE}")
    message(FATAL_ERROR "AWS_C_IOT_PATCH_FILE must identify the Device Client aws-c-iot patch")
endif ()

if (NOT DEFINED AWS_C_IOT_EXPECTED_COMMIT)
    message(FATAL_ERROR "AWS_C_IOT_EXPECTED_COMMIT must be set")
endif ()

if (NOT DEFINED AWS_C_IOT_ALLOW_SOURCE_RESET)
    set(AWS_C_IOT_ALLOW_SOURCE_RESET OFF)
endif ()

find_package(Git REQUIRED)

execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
        WORKING_DIRECTORY "${AWS_C_IOT_SOURCE_DIR}"
        RESULT_VARIABLE AWS_C_IOT_REV_PARSE_RESULT
        OUTPUT_VARIABLE AWS_C_IOT_ACTUAL_COMMIT
        ERROR_VARIABLE AWS_C_IOT_REV_PARSE_ERROR
        OUTPUT_STRIP_TRAILING_WHITESPACE)

if (NOT "${AWS_C_IOT_REV_PARSE_RESULT}" STREQUAL "0")
    message(FATAL_ERROR "Unable to inspect aws-c-iot HEAD: ${AWS_C_IOT_REV_PARSE_ERROR}")
endif ()

if (NOT "${AWS_C_IOT_ACTUAL_COMMIT}" STREQUAL "${AWS_C_IOT_EXPECTED_COMMIT}")
    message(FATAL_ERROR
            "The Device Client aws-c-iot patch expects ${AWS_C_IOT_EXPECTED_COMMIT}, "
            "but the SDK contains ${AWS_C_IOT_ACTUAL_COMMIT}")
endif ()

execute_process(
        COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=all
        WORKING_DIRECTORY "${AWS_C_IOT_SOURCE_DIR}"
        RESULT_VARIABLE AWS_C_IOT_STATUS_RESULT
        OUTPUT_VARIABLE AWS_C_IOT_STATUS_OUTPUT
        ERROR_VARIABLE AWS_C_IOT_STATUS_ERROR
        OUTPUT_STRIP_TRAILING_WHITESPACE)

if (NOT "${AWS_C_IOT_STATUS_RESULT}" STREQUAL "0")
    message(FATAL_ERROR "Unable to inspect aws-c-iot status: ${AWS_C_IOT_STATUS_ERROR}")
endif ()

execute_process(
        COMMAND "${GIT_EXECUTABLE}" apply --reverse --check --whitespace=nowarn "${AWS_C_IOT_PATCH_FILE}"
        WORKING_DIRECTORY "${AWS_C_IOT_SOURCE_DIR}"
        RESULT_VARIABLE AWS_C_IOT_REVERSE_CHECK_RESULT
        OUTPUT_VARIABLE AWS_C_IOT_REVERSE_CHECK_OUTPUT
        ERROR_VARIABLE AWS_C_IOT_REVERSE_CHECK_ERROR)

if ("${AWS_C_IOT_REVERSE_CHECK_RESULT}" STREQUAL "0")
    execute_process(
            COMMAND "${GIT_EXECUTABLE}" ls-files --others --exclude-standard
            WORKING_DIRECTORY "${AWS_C_IOT_SOURCE_DIR}"
            RESULT_VARIABLE AWS_C_IOT_UNTRACKED_RESULT
            OUTPUT_VARIABLE AWS_C_IOT_UNTRACKED_OUTPUT
            ERROR_VARIABLE AWS_C_IOT_UNTRACKED_ERROR
            OUTPUT_STRIP_TRAILING_WHITESPACE)

    if (NOT "${AWS_C_IOT_UNTRACKED_RESULT}" STREQUAL "0")
        message(FATAL_ERROR "Unable to inspect untracked aws-c-iot files: ${AWS_C_IOT_UNTRACKED_ERROR}")
    endif ()

    if ("${AWS_C_IOT_UNTRACKED_OUTPUT}" STREQUAL "")
        execute_process(
                COMMAND "${GIT_EXECUTABLE}"
                        -c diff.noprefix=false
                        -c diff.mnemonicPrefix=false
                        -c diff.context=3
                        -c diff.interHunkContext=0
                        -c diff.algorithm=myers
                        -c diff.indentHeuristic=true
                        -c diff.compactionHeuristic=false
                        -c diff.renames=false
                        diff
                        --binary
                        --full-index
                        --no-ext-diff
                        --no-color
                        --no-textconv
                        --no-renames
                        --src-prefix=a/
                        --dst-prefix=b/
                        --unified=3
                        --inter-hunk-context=0
                        --diff-algorithm=myers
                        --indent-heuristic
                        HEAD
                        --
                WORKING_DIRECTORY "${AWS_C_IOT_SOURCE_DIR}"
                RESULT_VARIABLE AWS_C_IOT_DIFF_RESULT
                OUTPUT_VARIABLE AWS_C_IOT_ACTUAL_PATCH
                ERROR_VARIABLE AWS_C_IOT_DIFF_ERROR)

        if (NOT "${AWS_C_IOT_DIFF_RESULT}" STREQUAL "0")
            message(FATAL_ERROR "Unable to inspect the patched aws-c-iot diff: ${AWS_C_IOT_DIFF_ERROR}")
        endif ()

        file(READ "${AWS_C_IOT_PATCH_FILE}" AWS_C_IOT_EXPECTED_PATCH)
        if ("${AWS_C_IOT_ACTUAL_PATCH}" STREQUAL "${AWS_C_IOT_EXPECTED_PATCH}")
            message(STATUS "Device Client aws-c-iot secure tunneling patch is already applied")
            return()
        endif ()
    endif ()
endif ()

if (NOT "${AWS_C_IOT_STATUS_OUTPUT}" STREQUAL "")
    if (AWS_C_IOT_ALLOW_SOURCE_RESET)
        message(STATUS "Resetting managed aws-c-iot source before applying Device Client patches")
        execute_process(
                COMMAND "${GIT_EXECUTABLE}" reset --hard "${AWS_C_IOT_EXPECTED_COMMIT}"
                WORKING_DIRECTORY "${AWS_C_IOT_SOURCE_DIR}"
                RESULT_VARIABLE AWS_C_IOT_RESET_RESULT
                ERROR_VARIABLE AWS_C_IOT_RESET_ERROR
                OUTPUT_QUIET)
        if (NOT "${AWS_C_IOT_RESET_RESULT}" STREQUAL "0")
            message(FATAL_ERROR "Unable to reset managed aws-c-iot source: ${AWS_C_IOT_RESET_ERROR}")
        endif ()

        execute_process(
                COMMAND "${GIT_EXECUTABLE}" clean -fdq
                WORKING_DIRECTORY "${AWS_C_IOT_SOURCE_DIR}"
                RESULT_VARIABLE AWS_C_IOT_CLEAN_RESULT
                ERROR_VARIABLE AWS_C_IOT_CLEAN_ERROR)
        if (NOT "${AWS_C_IOT_CLEAN_RESULT}" STREQUAL "0")
            message(FATAL_ERROR "Unable to clean managed aws-c-iot source: ${AWS_C_IOT_CLEAN_ERROR}")
        endif ()
    else ()
        message(FATAL_ERROR
                "Refusing to reset a modified external aws-c-iot source tree:\n"
                "${AWS_C_IOT_STATUS_OUTPUT}")
    endif ()
endif ()

execute_process(
        COMMAND "${GIT_EXECUTABLE}" apply --check --whitespace=nowarn "${AWS_C_IOT_PATCH_FILE}"
        WORKING_DIRECTORY "${AWS_C_IOT_SOURCE_DIR}"
        RESULT_VARIABLE AWS_C_IOT_PATCH_CHECK_RESULT
        OUTPUT_VARIABLE AWS_C_IOT_PATCH_CHECK_OUTPUT
        ERROR_VARIABLE AWS_C_IOT_PATCH_CHECK_ERROR)

if (NOT "${AWS_C_IOT_PATCH_CHECK_RESULT}" STREQUAL "0")
    message(FATAL_ERROR
            "The Device Client aws-c-iot patch cannot be applied.\n"
            "Apply check:\n${AWS_C_IOT_PATCH_CHECK_OUTPUT}${AWS_C_IOT_PATCH_CHECK_ERROR}\n"
            "Reverse check:\n${AWS_C_IOT_REVERSE_CHECK_OUTPUT}${AWS_C_IOT_REVERSE_CHECK_ERROR}")
endif ()

execute_process(
        COMMAND "${GIT_EXECUTABLE}" apply --whitespace=nowarn "${AWS_C_IOT_PATCH_FILE}"
        WORKING_DIRECTORY "${AWS_C_IOT_SOURCE_DIR}"
        RESULT_VARIABLE AWS_C_IOT_PATCH_RESULT
        OUTPUT_VARIABLE AWS_C_IOT_PATCH_OUTPUT
        ERROR_VARIABLE AWS_C_IOT_PATCH_ERROR)

if (NOT "${AWS_C_IOT_PATCH_RESULT}" STREQUAL "0")
    message(FATAL_ERROR "Unable to apply the Device Client aws-c-iot patch: ${AWS_C_IOT_PATCH_ERROR}")
endif ()

message(STATUS "Applied the Device Client aws-c-iot secure tunneling patch")
