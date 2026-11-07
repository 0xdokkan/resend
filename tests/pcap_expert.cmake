# Writes captures with resend-cli and asks tshark's expert system about
# them: Wireshark must find no errors or warnings, checksums included.
# Run with cmake -P, given CLI, TSHARK (empty when not found), EXAMPLES
# and OUT, a directory for the files.

if(NOT TSHARK)
    message("SKIPPED: tshark was not found")
    return()
endif()

file(MAKE_DIRECTORY "${OUT}")
# Each run: a name, then the command line, as a shell would split it.
set(runs
    "sample --count 3"
    "office --scenario '${EXAMPLES}/office-floor.json' --count 2 --ping Desk-1 10.0.0.22 --ping Desk-5 10.0.0.12 --ping Desk-1 10.0.0.23"
    "loop --scenario '${EXAMPLES}/switch-loop.json' --ping PC-A 192.168.1.20"
    "edits --count 4 --at 1.5s --link-down 2 --at 2.5s --link-up 2 --at 2.6s --power-off PC-B --at 2.7s --power-on PC-B"
    "cable --count 2 --pcap-link 2")
foreach(run IN LISTS runs)
    separate_arguments(parts UNIX_COMMAND "${run}")
    list(POP_FRONT parts name)
    set(capture "${OUT}/${name}.pcap")
    file(REMOVE "${capture}")
    execute_process(COMMAND "${CLI}" ${parts} --pcap "${capture}" OUTPUT_QUIET RESULT_VARIABLE status)
    # 0 or 1: every ping answered or not; anything else is a failure.
    if(NOT status MATCHES "^[01]$" OR NOT EXISTS "${capture}")
        message(FATAL_ERROR "${name}: resend-cli failed (${status})")
    endif()
    execute_process(COMMAND "${TSHARK}" -r "${capture}" -q -z expert -o ip.check_checksum:TRUE
        OUTPUT_VARIABLE report ERROR_VARIABLE errors RESULT_VARIABLE status)
    if(status)
        message(FATAL_ERROR "${name}: tshark failed (${status}): ${errors}")
    endif()
    if(report MATCHES "(Errors|Warns|Warnings) \\(")
        message(FATAL_ERROR "${name}: tshark reports problems:\n${report}")
    endif()
    execute_process(COMMAND "${TSHARK}" -r "${capture}" OUTPUT_VARIABLE frames)
    string(REGEX MATCHALL "\n" lines "${frames}")
    list(LENGTH lines count)
    message("${name}: ${count} frames, no errors or warnings")
endforeach()
