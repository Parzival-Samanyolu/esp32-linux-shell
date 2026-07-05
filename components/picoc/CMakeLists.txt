# PicoC — a very small C interpreter (vendored, jpoirier fork), ported to
# ESP-IDF with a custom platform layer (platform_esp32.c).
idf_component_register(
    SRCS
        "clibrary.c" "debug.c" "expression.c" "heap.c" "include.c" "lex.c"
        "parse.c" "platform.c" "table.c" "type.c" "variable.c"
        "platform_esp32.c"
        "cstdlib/ctype.c" "cstdlib/errno.c" "cstdlib/math.c" "cstdlib/stdbool.c"
        "cstdlib/stdio.c" "cstdlib/stdlib.c" "cstdlib/string.c" "cstdlib/time.c"
    INCLUDE_DIRS "."
)
# PicoC selects its platform via UNIX_HOST (POSIX-ish; matches ESP-IDF's newlib).
target_compile_definitions(${COMPONENT_LIB} PRIVATE UNIX_HOST)
# PicoC is old K&R-flavoured C: empty-paren function pointers and loose pointer
# casts that GCC 15 rejects by default. Build it as gnu11 + permissive, and
# silence the resulting warnings (third-party code).
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu11 -fpermissive -w)
