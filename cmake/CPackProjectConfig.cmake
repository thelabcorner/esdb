# Release-archive reproducibility policy for ESDB 0.2.0.
#
# CPack's Archive generator honors SOURCE_DATE_EPOCH for ZIP entry timestamps.
# Keep this epoch tied to the 0.2.0 release date (2026-09-25 00:00:00 UTC).
# Update it intentionally when preparing a later release.
if(CPACK_GENERATOR STREQUAL "ZIP")
    set(ENV{SOURCE_DATE_EPOCH} "1790294400")
endif()
