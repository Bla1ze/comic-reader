cmake_minimum_required(VERSION 3.5)

# Comic Reader: comics and manuals on an AtGames Legends cabinet. Builds with
# the AtGames External Applications SDK toolchain; MuPDF and libarchive come
# prebuilt from tools/build-deps.sh (static, the same compiler).
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()
project(comic-reader LANGUAGES C CXX)

set(ATGAMES_APP_NAME "comic-reader" CACHE STRING "External Application folder/output name")
set(COMIC_DEPS "/workspace/comic-reader-deps/prefix" CACHE PATH "MuPDF + libarchive from tools/build-deps.sh")

find_path(SDL2_INCLUDE_DIR NAMES SDL.h PATHS /usr/aarch64-linux-gnu/include/SDL2 NO_DEFAULT_PATH)
find_library(SDL2_LIBRARY NAMES SDL2 PATHS /usr/aarch64-linux-gnu/lib NO_DEFAULT_PATH)
if(NOT SDL2_INCLUDE_DIR OR NOT SDL2_LIBRARY)
    message(FATAL_ERROR "SDL2 was not found. Rebuild the SDK Docker image.")
endif()
foreach(lib mupdf mupdf-third archive)
    if(NOT EXISTS "${COMIC_DEPS}/lib/lib${lib}.a")
        message(FATAL_ERROR "${COMIC_DEPS}/lib/lib${lib}.a is missing: run tools/build-deps.sh first.")
    endif()
endforeach()

# Fonts embedded in the ELF: Comic Neue Bold (body text) and Bangers (lettering).
set(FONT_ASSET_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts")
set(FONT_OBJECTS "")
foreach(font ComicNeue-Bold Bangers-Regular)
    set(obj "${CMAKE_CURRENT_BINARY_DIR}/${font}.o")
    add_custom_command(
        OUTPUT "${obj}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${FONT_ASSET_DIR}/${font}.ttf" "${CMAKE_CURRENT_BINARY_DIR}/${font}.ttf"
        COMMAND aarch64-linux-gnu-objcopy -I binary -O elf64-littleaarch64 -B aarch64 ${font}.ttf ${font}.o
        DEPENDS "${FONT_ASSET_DIR}/${font}.ttf"
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
        COMMENT "Embedding ${font}")
    list(APPEND FONT_OBJECTS "${obj}")
endforeach()
set_source_files_properties(${FONT_OBJECTS} PROPERTIES GENERATED TRUE EXTERNAL_OBJECT TRUE)

add_executable(${ATGAMES_APP_NAME}
    ${FONT_OBJECTS}
    src/controls/Controls.cpp
    src/App.cpp
    src/AppFont.cpp
    src/Covers.cpp
    src/Doc.c
    src/compat.c
    src/Gfx.cpp
    src/Library.cpp
    src/Pages.cpp
    src/PanelFont.cpp
    src/Panels.cpp
    src/Theme.cpp
    src/Transfer.cpp
    src/main.cpp
)
set_property(TARGET ${ATGAMES_APP_NAME} PROPERTY CXX_STANDARD 14)
set_property(TARGET ${ATGAMES_APP_NAME} PROPERTY CXX_STANDARD_REQUIRED ON)
set_property(TARGET ${ATGAMES_APP_NAME} PROPERTY C_STANDARD 99)
target_include_directories(${ATGAMES_APP_NAME} PRIVATE
    ${SDL2_INCLUDE_DIR}
    ${COMIC_DEPS}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/src/vendor)
target_link_libraries(${ATGAMES_APP_NAME} PRIVATE
    ${COMIC_DEPS}/lib/libmupdf.a ${COMIC_DEPS}/lib/libmupdf-third.a ${COMIC_DEPS}/lib/libarchive.a
    ${SDL2_LIBRARY} pthread dl m)
# Not stripped: a crash backtrace in the log then names functions. (-rdynamic
# exports them to backtrace_symbols.)
set_target_properties(${ATGAMES_APP_NAME} PROPERTIES LINK_FLAGS "-rdynamic")

set(DIST_APP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../dist/external/${ATGAMES_APP_NAME}")
set(METADATA_ASSET_DIR "${CMAKE_CURRENT_SOURCE_DIR}/assets/metadata")
set_target_properties(${ATGAMES_APP_NAME} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${DIST_APP_DIR}" SUFFIX ".elf")
add_custom_command(TARGET ${ATGAMES_APP_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${METADATA_ASSET_DIR}/${ATGAMES_APP_NAME}.png" "${DIST_APP_DIR}/${ATGAMES_APP_NAME}.png"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${METADATA_ASSET_DIR}/${ATGAMES_APP_NAME}.xml" "${DIST_APP_DIR}/${ATGAMES_APP_NAME}.xml"
    # License texts that travel with the binary.
    COMMAND ${CMAKE_COMMAND} -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/licenses" "${DIST_APP_DIR}/licenses")
