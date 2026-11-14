// The Wi-Fi setup page, up only while the board is in setup mode (netcfg.h): a web server on port 80 of the setup
// hotspot and a DNS server that answers every name with the board's address, so a phone or laptop that joins the
// hotspot opens the page by itself (captive portal). German or English by the browser's Accept-Language.
// It runs on its own task: a slow or stuck browser never holds up loop(). The page only hands a network to
// net_setup_submit(); it never shows a stored password or the hotspot's own.
#pragma once

void portal_start();  // from loop(), once the setup hotspot is up
void portal_stop();   // from loop(): closes port 80 and the DNS server (waits up to 2 s for a request in flight)
