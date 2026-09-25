# Normalize generated ZIP metadata after CPack has written the archive.
find_program(_ESDB_PYTHON_EXECUTABLE NAMES python3 python)
if(NOT _ESDB_PYTHON_EXECUTABLE)
    message(FATAL_ERROR
        "Python 3 is required to normalize ESDB release ZIP metadata")
endif()

foreach(_esdb_package IN LISTS CPACK_PACKAGE_FILES)
    if(_esdb_package MATCHES "\\.zip$")
        execute_process(
            COMMAND
                "${_ESDB_PYTHON_EXECUTABLE}"
                "${CMAKE_CURRENT_LIST_DIR}/../tools/normalize-zip.py"
                "${_esdb_package}"
            RESULT_VARIABLE _esdb_normalize_result
            OUTPUT_VARIABLE _esdb_normalize_output
            ERROR_VARIABLE _esdb_normalize_error
        )
        if(NOT _esdb_normalize_result EQUAL 0)
            message(FATAL_ERROR
                "Failed to normalize ${_esdb_package}: "
                "${_esdb_normalize_output}${_esdb_normalize_error}")
        endif()
    endif()
endforeach()
