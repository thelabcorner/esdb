if(NOT DEFINED ESDB_SQLITE_SOURCE OR
   NOT DEFINED ESDB_ZIPVFS_SOURCE OR
   NOT DEFINED ESDB_OUTPUT)
    message(FATAL_ERROR
        "CombineZipvfs.cmake requires ESDB_SQLITE_SOURCE, "
        "ESDB_ZIPVFS_SOURCE, and ESDB_OUTPUT")
endif()

foreach(_path IN ITEMS "${ESDB_SQLITE_SOURCE}" "${ESDB_ZIPVFS_SOURCE}")
    if(NOT EXISTS "${_path}")
        message(FATAL_ERROR "ZIPVFS input does not exist: ${_path}")
    endif()
endforeach()

get_filename_component(_output_dir "${ESDB_OUTPUT}" DIRECTORY)
file(MAKE_DIRECTORY "${_output_dir}")

# ZIPVFS must be appended to the SQLite amalgamation in the same translation
# unit. Keep the licensed source external; only the generated build-tree file
# contains it.
file(READ "${ESDB_SQLITE_SOURCE}" _sqlite)
file(READ "${ESDB_ZIPVFS_SOURCE}" _zipvfs)
file(WRITE "${ESDB_OUTPUT}"
    "${_sqlite}\n/* ---- ESDB private licensed ZIPVFS append ---- */\n${_zipvfs}\n")
unset(_sqlite)
unset(_zipvfs)
