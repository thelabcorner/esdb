# Release-archive reproducibility policy for ESDB 0.1.0.
#
# CPack's Archive generator honors SOURCE_DATE_EPOCH for ZIP entry timestamps.
# Keep this epoch tied to the 0.1.0 release date (2026-09-24 00:00:00 UTC).
# Update it intentionally when preparing a later release.
if(CPACK_GENERATOR STREQUAL "ZIP")
    set(ENV{SOURCE_DATE_EPOCH} "1790208000")
endif()
