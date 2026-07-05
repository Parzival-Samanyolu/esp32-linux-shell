#pragma once

// Simple in-RAM kernel-message ring buffer (the `dmesg` command reads it).
// Thread-safe; lines are timestamped with seconds since boot.

void dmesg_init(void);

// printf-style; adds one line to the ring buffer.
void dmesg_add(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Stream the whole buffer to a connected socket (used by the `dmesg` command).
void dmesg_dump(int sock);
