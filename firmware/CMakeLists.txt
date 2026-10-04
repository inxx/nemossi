cmake_minimum_required(VERSION 3.16)

# Never silently target a different chip when configuring a fresh checkout.
set(IDF_TARGET esp32s3 CACHE STRING "Nemossi hardware target")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(nemossi)
