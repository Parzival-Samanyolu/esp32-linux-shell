# Duktape embeddable JavaScript engine (vendored amalgamation, v2.7.0).
idf_component_register(
    SRCS "duktape.c"
    INCLUDE_DIRS "."
)
# Third-party code: don't apply our -Werror strictness to it.
target_compile_options(${COMPONENT_LIB} PRIVATE -w)
