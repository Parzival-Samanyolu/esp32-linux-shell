#pragma once
// Route PicoC interpreter output to a caller-supplied per-character sink
// (the shell layer sends it to the TCP socket).
typedef void (*picoc_out_fn)(char c);
void picoc_set_output(picoc_out_fn fn);

// Emit one character of interpreted-program output (stdout/stderr) to the sink.
void picoc_emit(char c);
