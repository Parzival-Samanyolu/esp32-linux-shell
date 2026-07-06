idf_component_register(SRCS "qrcodegen.c" INCLUDE_DIRS ".")
target_compile_options(${COMPONENT_LIB} PRIVATE -w)
