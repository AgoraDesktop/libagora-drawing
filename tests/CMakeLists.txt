# libagora-drawing's own suite.
#
# It links the library and Core Foundation, because the doors it drives are declared with Core Foundation
# types. lcms2, pixman, libpng, libjpeg and FreeType are the LIBRARY's dependencies and not the suite's:
# nothing here reaches through the library into the engines.

add_executable(drawing drawing.c)
target_link_libraries(drawing PRIVATE Drawing CoreFoundation::CoreFoundation)

add_test(NAME drawing COMMAND drawing)
