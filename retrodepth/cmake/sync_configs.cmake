# sync_configs.cmake
# Copies all files from SRC_DIR to DST_DIR.
# settings.json is skipped if it already exists at the destination,
# preserving any local user edits across rebuilds.

file(MAKE_DIRECTORY "${DST_DIR}")
file(GLOB src_files "${SRC_DIR}/*")
foreach(src_file ${src_files})
    get_filename_component(fname "${src_file}" NAME)
    set(dst_file "${DST_DIR}/${fname}")
    if(fname STREQUAL "settings.json" AND EXISTS "${dst_file}")
        # Leave the user's edited settings.json in place
    else()
        file(COPY "${src_file}" DESTINATION "${DST_DIR}")
    endif()
endforeach()
