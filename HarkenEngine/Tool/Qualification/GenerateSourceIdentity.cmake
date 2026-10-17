set(_rtq_commit "unknown")
set(_rtq_tracked_clean 0)
set(_rtq_untracked_present 0)
set(_rtq_git_dirty 1)
set(_rtq_commit_known 0)
set(_rtq_tracked_status_known 0)
set(_rtq_untracked_status_known 0)

if(NOT HARKEN_RTQ_GIT_EXECUTABLE STREQUAL "")
    execute_process(
        COMMAND "${HARKEN_RTQ_GIT_EXECUTABLE}" rev-parse HEAD
        WORKING_DIRECTORY "${HARKEN_RTQ_SOURCE_DIR}"
        RESULT_VARIABLE _rtq_commit_result
        OUTPUT_VARIABLE _rtq_commit_output
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    string(LENGTH "${_rtq_commit_output}" _rtq_commit_length)
    if(_rtq_commit_result EQUAL 0 AND
       _rtq_commit_length EQUAL 40 AND
       _rtq_commit_output MATCHES "^[0-9a-fA-F]+$")
        string(TOLOWER "${_rtq_commit_output}" _rtq_commit_candidate)
        set(_rtq_commit_known 1)
    endif()

    execute_process(
        COMMAND "${HARKEN_RTQ_GIT_EXECUTABLE}" status --porcelain
                --untracked-files=no
        WORKING_DIRECTORY "${HARKEN_RTQ_SOURCE_DIR}"
        RESULT_VARIABLE _rtq_tracked_result
        OUTPUT_VARIABLE _rtq_tracked_output
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(_rtq_tracked_result EQUAL 0)
        set(_rtq_tracked_status_known 1)
    endif()

    execute_process(
        COMMAND "${HARKEN_RTQ_GIT_EXECUTABLE}"
                ls-files --others --exclude-standard
        WORKING_DIRECTORY "${HARKEN_RTQ_SOURCE_DIR}"
        RESULT_VARIABLE _rtq_untracked_result
        OUTPUT_VARIABLE _rtq_untracked_output
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(_rtq_untracked_result EQUAL 0)
        set(_rtq_untracked_status_known 1)
    endif()
endif()

if(_rtq_commit_known AND
   _rtq_tracked_status_known AND
   _rtq_untracked_status_known)
    set(_rtq_commit "${_rtq_commit_candidate}")
    if(_rtq_tracked_output STREQUAL "")
        set(_rtq_tracked_clean 1)
    endif()
    if(NOT _rtq_untracked_output STREQUAL "")
        set(_rtq_untracked_present 1)
    endif()
    if(_rtq_tracked_clean AND NOT _rtq_untracked_present)
        set(_rtq_git_dirty 0)
    endif()
endif()

string(CONCAT _rtq_content
"#ifndef HARKEN_RTQ_SOURCE_IDENTITY_H\n"
"#define HARKEN_RTQ_SOURCE_IDENTITY_H\n"
"#define HARKEN_RTQ_GIT_COMMIT \"${_rtq_commit}\"\n"
"#define HARKEN_RTQ_GIT_DIRTY ${_rtq_git_dirty}\n"
"#define HARKEN_RTQ_TRACKED_WORKTREE_CLEAN ${_rtq_tracked_clean}\n"
"#define HARKEN_RTQ_UNTRACKED_ENTRIES_PRESENT ${_rtq_untracked_present}\n"
"#endif\n")

get_filename_component(_rtq_output_dir "${HARKEN_RTQ_OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${_rtq_output_dir}")
if(EXISTS "${HARKEN_RTQ_OUTPUT}")
    file(READ "${HARKEN_RTQ_OUTPUT}" _rtq_existing)
else()
    set(_rtq_existing "")
endif()
if(NOT _rtq_existing STREQUAL _rtq_content)
    file(WRITE "${HARKEN_RTQ_OUTPUT}" "${_rtq_content}")
endif()
