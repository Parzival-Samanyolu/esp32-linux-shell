cmake_minimum_required(VERSION 3.16.0)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(deneyap_kart)

# The vendored pocketpy interpreter trips several -Werror warnings on 32-bit
# (type-limits, pointer<->int casts, ...). Silence warnings for THAT component
# only, so we don't patch a managed component and keep our own code strict.
idf_component_get_property(pkpy_lib nicolaielectronics__pocketpy COMPONENT_LIB)
target_compile_options(${pkpy_lib} PRIVATE -w)
