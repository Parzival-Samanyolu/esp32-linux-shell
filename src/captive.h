#pragma once

// Start the captive-portal DNS hijack: every DNS query from a hotspot client is
// answered with the board's AP IP, so joining the hotspot pops up the desktop.
// (Pair with the HTTP 404->/gui redirect registered in fileserver.c.)
void captive_portal_start(void);
