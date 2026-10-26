# Copyright © 2026 Kyle J. Cardoza.
# SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
# SPDX-License-Identifier: GPL-3.0-or-later

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

# THE GLYPH BITMAP CACHE: a hit answers what the miss answered (bytes and metrics), caching off is identical to
# caching on, an evicted entry is re-rendered rather than remembered wrong, a released face's address cannot be
# reused as a key, and a string drawn through CGContext is the same pixels cold and warm. The bundled font
# directory is handed over the same way `fonts` gets it, because ctest runs against the build tree.
add_executable(glyph_cache glyph_cache.c)
target_link_libraries(glyph_cache PRIVATE Drawing CoreFoundation::CoreFoundation)
target_compile_definitions(glyph_cache PRIVATE
  AGORA_TEST_FONT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../fonts")

add_test(NAME glyph-cache COMMAND glyph_cache)

# THE ADVANCE CACHE: values compared against a font whose cache is cold, the per-font counters proving the second
# ask is the cached one, the all-or-nothing refusal leaving the caller's array untouched while keeping what loaded,
# and a batch agreeing with a trickle. The face comes from the bundled directory for the same reason `fonts` does.
add_executable(glyph_advances glyph_advances.c)
target_link_libraries(glyph_advances PRIVATE Drawing CoreFoundation::CoreFoundation)
target_compile_definitions(glyph_advances PRIVATE
  AGORA_TEST_FONT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../fonts")

add_test(NAME glyph-advances COMMAND glyph_advances)
# The library's own dependencies (libobjc and Core Foundation's) live in the install prefix, and DT_RUNPATH is
# NOT inherited by a transitive dependency — so the prefix's lib directory has to be on `LD_LIBRARY_PATH` for
# the hops to resolve. The directories come from imported targets rather than being written down.
set_tests_properties(drawing PROPERTIES ENVIRONMENT
  "LD_LIBRARY_PATH=$<TARGET_FILE_DIR:Drawing>:$<TARGET_FILE_DIR:CoreFoundation::CoreFoundation>")
set_tests_properties(window-levels PROPERTIES ENVIRONMENT
  "LD_LIBRARY_PATH=$<TARGET_FILE_DIR:Drawing>:$<TARGET_FILE_DIR:CoreFoundation::CoreFoundation>")
set_tests_properties(fonts PROPERTIES ENVIRONMENT
  "LD_LIBRARY_PATH=$<TARGET_FILE_DIR:Drawing>:$<TARGET_FILE_DIR:CoreFoundation::CoreFoundation>")
set_tests_properties(glyph-cache PROPERTIES ENVIRONMENT
  "LD_LIBRARY_PATH=$<TARGET_FILE_DIR:Drawing>:$<TARGET_FILE_DIR:CoreFoundation::CoreFoundation>")
set_tests_properties(glyph-advances PROPERTIES ENVIRONMENT
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

# **CFF, AS A PDF TYPE 1 FONT.** The face is the host's (this tree vendors none), so the test SKIPS with 77 when no
# candidate is present. It checks the /FontFile3 /Type1C structure, /Differences through the Tj content, the rendered
# ink, and that 300 glyphs split across more than one 255-code shard.
add_executable(pdf_cff pdf_cff.c)
target_link_libraries(pdf_cff PRIVATE Drawing CoreFoundation::CoreFoundation)

add_test(NAME pdf-cff COMMAND pdf_cff)
set_tests_properties(pdf-cff PROPERTIES SKIP_RETURN_CODE 77 ENVIRONMENT
  "LD_LIBRARY_PATH=$<TARGET_FILE_DIR:Drawing>:$<TARGET_FILE_DIR:CoreFoundation::CoreFoundation>")

# **EPS, READ BACK BY GHOSTSCRIPT.** The EPS writer is proved by another PostScript implementation, not by its
# own text: draw a rectangle, a stroke, text outlines, an image and a gradient, close the file, let `gs` render it
# to a PPM and read the pixels. The case SKIPS with 77 when the host has no `gs`.
add_executable(eps_write eps_write.c)
target_link_libraries(eps_write PRIVATE Drawing CoreFoundation::CoreFoundation)

add_test(NAME eps-write COMMAND eps_write)
set_tests_properties(eps-write PROPERTIES SKIP_RETURN_CODE 77 ENVIRONMENT
  "LD_LIBRARY_PATH=$<TARGET_FILE_DIR:Drawing>:$<TARGET_FILE_DIR:CoreFoundation::CoreFoundation>")

# **THE FILL'S OWN HEAP COST.** `cg_traps_for_path` owns two arrays — the flattened vertices and the
# clipped outline — and must release both. This case interposes the allocation doors and measures
# outstanding bytes across 256 fills, 256 clips, and a build-only control that must also retain nothing.
# The accounting is glibc's (`__libc_malloc`, `malloc_usable_size`), so the case SKIPS with 77 elsewhere.
add_executable(flatten_memory flatten_memory.c)
target_link_libraries(flatten_memory PRIVATE Drawing CoreFoundation::CoreFoundation)

add_test(NAME flatten-memory COMMAND flatten_memory)
set_tests_properties(flatten-memory PROPERTIES SKIP_RETURN_CODE 77 ENVIRONMENT
  "LD_LIBRARY_PATH=$<TARGET_FILE_DIR:Drawing>:$<TARGET_FILE_DIR:CoreFoundation::CoreFoundation>")
