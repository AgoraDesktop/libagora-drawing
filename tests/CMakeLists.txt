# libagora-drawing's own suite.
#
# It links the library and Core Foundation, because the doors it drives are declared with Core Foundation
# types. lcms2, pixman, libpng, libjpeg and FreeType are the LIBRARY's dependencies and not the suite's:
# nothing here reaches through the library into the engines.

add_executable(drawing drawing.c)
target_link_libraries(drawing PRIVATE Drawing CoreFoundation::CoreFoundation)

add_test(NAME drawing COMMAND drawing)

# F4's measurement: the Core Graphics window-level vocabulary, and its agreement with NSWindow.h's values.
add_executable(window_levels window_levels.c)
target_link_libraries(window_levels PRIVATE Drawing CoreFoundation::CoreFoundation)

add_test(NAME window-levels COMMAND window_levels)

# THE DEFAULT FONT SET AND THE macOS COMPATIBILITY NAMES. `FN_FONT_PATH` points at the SOURCE tree's fonts, because
# the installed copy does not exist yet when ctest runs against a build tree; the case is otherwise the real name
# door. The source directory is passed as a compile definition rather than written into the test, so a relocated
# checkout still runs.
add_executable(fonts fonts.c)
target_link_libraries(fonts PRIVATE Drawing CoreFoundation::CoreFoundation)
target_compile_definitions(fonts PRIVATE
  AGORA_TEST_FONT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../fonts")

add_test(NAME fonts COMMAND fonts)
# The library's own dependencies (libobjc and Core Foundation's) live in the install prefix, and DT_RUNPATH is
# NOT inherited by a transitive dependency — so the prefix's lib directory has to be on `LD_LIBRARY_PATH` for
# the hops to resolve. The directories come from imported targets rather than being written down.
set_tests_properties(drawing PROPERTIES ENVIRONMENT
  "LD_LIBRARY_PATH=$<TARGET_FILE_DIR:Drawing>:$<TARGET_FILE_DIR:CoreFoundation::CoreFoundation>")
set_tests_properties(window-levels PROPERTIES ENVIRONMENT
  "LD_LIBRARY_PATH=$<TARGET_FILE_DIR:Drawing>:$<TARGET_FILE_DIR:CoreFoundation::CoreFoundation>")
set_tests_properties(fonts PROPERTIES ENVIRONMENT
  "LD_LIBRARY_PATH=$<TARGET_FILE_DIR:Drawing>:$<TARGET_FILE_DIR:CoreFoundation::CoreFoundation>")

# THE PDF READER: two fixtures built byte by byte (a classic cross-reference table and a PDF 1.5 xref stream with
# an object stream), every structural door, and the page transform's rotated geometry. The fixture builder and the
# library are the only two things here; zlib is the library's PRIVATE dependency, reached through the .so.
add_executable(pdf pdf.c)
target_link_libraries(pdf PRIVATE Drawing CoreFoundation::CoreFoundation)

add_test(NAME pdf COMMAND pdf)

set_tests_properties(pdf PROPERTIES ENVIRONMENT
  "LD_LIBRARY_PATH=$<TARGET_FILE_DIR:Drawing>:$<TARGET_FILE_DIR:CoreFoundation::CoreFoundation>")

# THE PDF CONTEXT'S ROUND TRIP: draw into a CGPDFContext, close it, reopen it with CGPDFDocument, render it with
# CGContextDrawPDFPage and read the pixels back. The writer and the reader are proved against each other.
add_executable(pdf_write pdf_write.c)
target_link_libraries(pdf_write PRIVATE Drawing CoreFoundation::CoreFoundation)

add_test(NAME pdf-write COMMAND pdf_write)

set_tests_properties(pdf-write PROPERTIES ENVIRONMENT
  "LD_LIBRARY_PATH=$<TARGET_FILE_DIR:Drawing>:$<TARGET_FILE_DIR:CoreFoundation::CoreFoundation>")
