# libagora-drawing's own suite.
#
# It links the library and Core Foundation, because the doors it drives are declared with Core Foundation
# types. lcms2, pixman, libpng, libjpeg and FreeType are the LIBRARY's dependencies and not the suite's:
# nothing here reaches through the library into the engines.

add_executable(drawing drawing.c)
target_link_libraries(drawing PRIVATE Drawing CoreFoundation::CoreFoundation)

add_test(NAME drawing COMMAND drawing)
# The library's own dependencies (libobjc and Core Foundation's) live in the install prefix, and DT_RUNPATH is
# NOT inherited by a transitive dependency — so the prefix's lib directory has to be on `LD_LIBRARY_PATH` for
# the hops to resolve. The directories come from imported targets rather than being written down.
set_tests_properties(drawing PROPERTIES ENVIRONMENT
  "LD_LIBRARY_PATH=$<TARGET_FILE_DIR:Drawing>:$<TARGET_FILE_DIR:CoreFoundation::CoreFoundation>")
