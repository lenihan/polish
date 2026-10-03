# Stamps the current commit, working-tree state and build time into
# BuildInfo.cpp, so a running binary can say exactly which build it is.
#
# Run as a script (cmake -P), both at configure time and again before
# every build -- the second part is the point. A stamp generated only at
# configure time goes stale the moment you rebuild without reconfiguring,
# which is precisely the case it exists to tell apart: two builds of the
# same version with different code in them.
#
# configure_file leaves the output untouched when the content would be
# identical, so this only triggers a recompile when something it reports
# actually changed. The build time changes every build, so in practice
# that is every build -- which is why only this one small file is
# generated, and why it is linked into the executables rather than into
# polish_core: a rebuild then costs one tiny translation unit and a
# relink, and never touches the test binary.
#
# Expects: SRC, TEMPLATE, OUT, POLISH_VERSION.

set(POLISH_COMMIT "unknown")
set(POLISH_MODIFIED_SUFFIX "")
set(POLISH_MODIFIED_BOOL "false")

find_package(Git QUIET)
if(Git_FOUND)
    execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --short=7 HEAD
        WORKING_DIRECTORY "${SRC}"
        OUTPUT_VARIABLE _polish_sha
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE _polish_sha_result)
    if(_polish_sha_result EQUAL 0 AND NOT _polish_sha STREQUAL "")
        set(POLISH_COMMIT "${_polish_sha}")
        # --untracked-files=no on purpose: a stray scratch file in the
        # tree says nothing about what went into the binary, and having
        # the stamp cry "modified" over one would train you to ignore it.
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=no
            WORKING_DIRECTORY "${SRC}"
            OUTPUT_VARIABLE _polish_status
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        if(NOT _polish_status STREQUAL "")
            set(POLISH_MODIFIED_SUFFIX "+")
            set(POLISH_MODIFIED_BOOL "true")
        endif()
    endif()
endif()

# Local time, not UTC: this is read next to a wall clock while working
# out which of two builds is in front of you.
string(TIMESTAMP POLISH_BUILD_TIME "%Y-%m-%d %H:%M")

configure_file("${TEMPLATE}" "${OUT}" @ONLY)
