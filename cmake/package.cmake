cmake_minimum_required(VERSION 3.24)

foreach(required IN ITEMS MODE VERSION PLUGIN_FILE INI_FILE README_FILE
                          MINHOOK_LICENSE_FILE OUTPUT_DIR ARCHIVE_FILE)
    if(NOT DEFINED ${required} OR "${${required}}" STREQUAL "")
        message(FATAL_ERROR "Missing packaging argument: ${required}")
    endif()
endforeach()

if(NOT MODE STREQUAL "plugin-only" AND NOT MODE STREQUAL "loader-bundled")
    message(FATAL_ERROR "Unknown package mode: ${MODE}")
endif()

foreach(input IN ITEMS PLUGIN_FILE INI_FILE README_FILE MINHOOK_LICENSE_FILE)
    if(NOT EXISTS "${${input}}")
        message(FATAL_ERROR "Packaging input does not exist: ${${input}}")
    endif()
endforeach()

if(MODE STREQUAL "loader-bundled")
    foreach(input IN ITEMS LOADER_FILE LOADER_LICENSE_FILE)
        if(NOT DEFINED ${input} OR "${${input}}" STREQUAL "" OR
           NOT EXISTS "${${input}}")
            message(FATAL_ERROR "Loader packaging input does not exist: ${input}")
        endif()
    endforeach()
    # Only the verified x64 Ultimate ASI Loader v9.7.4 binary may be bundled.
    file(SHA512 "${LOADER_FILE}" loader_hash)
    string(TOUPPER "${loader_hash}" loader_hash)
    if(NOT loader_hash STREQUAL
       "840C97B7B3ED40DE3D4E6372D9BD43B022796F4EA57C71969B5DED978B344ED5A70579318337877B9AAD8C6F0E74CE19B32F1AC5D4C5E5679D5F4682819937FA")
        message(FATAL_ERROR
            "Loader binary is not the verified x64 Ultimate ASI Loader v9.7.4")
    endif()
endif()

file(READ "${README_FILE}" readme_contents)
set(required_misc_line
    "For more game fixes, visit https://slop-blog.enkhayzomachines.net/fixes :)")
string(FIND "${readme_contents}" "Misc" misc_heading_offset)
string(FIND "${readme_contents}" "${required_misc_line}" misc_line_offset)
if(misc_heading_offset EQUAL -1 OR misc_line_offset EQUAL -1)
    message(FATAL_ERROR "Release README must retain the Misc section and fixes URL")
endif()

file(REMOVE_RECURSE "${OUTPUT_DIR}")
file(MAKE_DIRECTORY "${OUTPUT_DIR}")
file(COPY_FILE "${PLUGIN_FILE}"
     "${OUTPUT_DIR}/AmnesiaTheBunker.FPSFix.asi" ONLY_IF_DIFFERENT)
file(COPY_FILE "${INI_FILE}"
     "${OUTPUT_DIR}/AmnesiaTheBunker.FPSFix.ini" ONLY_IF_DIFFERENT)
file(COPY_FILE "${README_FILE}" "${OUTPUT_DIR}/README.txt" ONLY_IF_DIFFERENT)
file(COPY_FILE "${MINHOOK_LICENSE_FILE}"
     "${OUTPUT_DIR}/MINHOOK-LICENSE.txt" ONLY_IF_DIFFERENT)

set(package_files
    AmnesiaTheBunker.FPSFix.asi
    AmnesiaTheBunker.FPSFix.ini
    README.txt
    MINHOOK-LICENSE.txt
)

if(MODE STREQUAL "loader-bundled")
    file(COPY_FILE "${LOADER_FILE}" "${OUTPUT_DIR}/version.dll"
         ONLY_IF_DIFFERENT)
    file(COPY_FILE "${LOADER_LICENSE_FILE}"
         "${OUTPUT_DIR}/ULTIMATE-ASI-LOADER-LICENSE.txt" ONLY_IF_DIFFERENT)
    list(APPEND package_files version.dll ULTIMATE-ASI-LOADER-LICENSE.txt)
endif()

set(checksums "")
foreach(file IN LISTS package_files)
    file(SHA256 "${OUTPUT_DIR}/${file}" hash)
    string(TOUPPER "${hash}" hash)
    string(APPEND checksums "${hash}  ${file}\n")
endforeach()
file(WRITE "${OUTPUT_DIR}/SHA256SUMS.txt" "${checksums}")
list(APPEND package_files SHA256SUMS.txt)

get_filename_component(archive_dir "${ARCHIVE_FILE}" DIRECTORY)
file(MAKE_DIRECTORY "${archive_dir}")
file(REMOVE "${ARCHIVE_FILE}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E tar cf "${ARCHIVE_FILE}" --format=zip -- ${package_files}
    WORKING_DIRECTORY "${OUTPUT_DIR}"
    COMMAND_ERROR_IS_FATAL ANY
)

message(STATUS "Packaged ${MODE}: ${ARCHIVE_FILE}")
