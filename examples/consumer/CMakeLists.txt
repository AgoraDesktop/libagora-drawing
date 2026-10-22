# Copyright © 2026 Kyle J. Cardoza.
# SPDX-FileCopyrightText: 2026 Kyle J. Cardoza
# SPDX-License-Identifier: GPL-3.0-or-later

# A CLIENT OF libagora-drawing, built AS ITS OWN PROJECT against the INSTALLED prefix.
#
# THIS IS NOT PART OF THE LIBRARY'S BUILD and is not added by add_subdirectory: it is what somebody outside
# this repository would write, and it is here because a client that exists finds things a test suite cannot.
# The first version of it found that Core Foundation was linked PRIVATE -- a client that links only
# AgoraDrawing::Drawing could not reach CFArrayCreate -- within minutes of existing, after nineteen suite
# cases had driven the library happily through its own build tree.
#
# To build and run it:
#
#   cmake -S examples/consumer -B <build> -DCMAKE_PREFIX_PATH=<prefix> -DCMAKE_C_COMPILER=/usr/bin/clang-19
#   cmake --build <build>
#   LD_LIBRARY_PATH=<prefix>/lib <build>/client
#
# THE COMPILER IS NAMED BECAUSE IT HAS TO BE: the target exports -fblocks on its interface, since Core
# Foundation's headers are spelled with blocks, and a client that lets CMake choose gets gcc and
# "unrecognized command-line option '-fblocks'". The package config now refuses that combination with a
# message rather than letting the linker explain it.
cmake_minimum_required(VERSION 3.24)
project(DrawingClient LANGUAGES C)

find_package(AgoraDrawing CONFIG REQUIRED)

add_executable(client main.c)
target_link_libraries(client PRIVATE AgoraDrawing::Drawing)
