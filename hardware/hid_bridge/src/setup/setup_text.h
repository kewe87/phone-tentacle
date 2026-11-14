#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Text helpers of the Wi-Fi setup page (setup_portal.cpp). Network names come from the air: anyone nearby picks
// them, so they are escaped wherever they are shown and travel back as hex.

// Escapes for HTML text and attribute values (& < > " '); control characters become '?'. Always NUL-terminated,
// never cut inside an entity. Returns the length written.
size_t setup_html_esc(const char *in, char *out, size_t cap);
// Escapes for a JSON string (and < > & as \u00XX, so the result is also safe inside a <script>).
size_t setup_json_esc(const char *in, char *out, size_t cap);

// Network names in the form: "x" + two hex digits per byte. Decoding fails on anything else, on an empty name, a
// NUL byte, or more than 32 bytes.
size_t setup_hex_enc(const char *in, char *out, size_t cap);
bool setup_hex_dec(const char *in, char *out, size_t cap);

enum { SETUP_OK = 0, SETUP_BAD_SSID, SETUP_BAD_PASS };
// A name of 1-32 bytes; a password of 8-63 bytes, or none for an open network; no control characters.
int setup_check(const char *ssid, const char *pass);

// True if the browser's Accept-Language prefers German over English (English is the default).
bool setup_lang_de(const char *accept_language);

#ifdef __cplusplus
}
#endif
