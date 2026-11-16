// Where mbedtls takes its memory. The Arduino core's ESP-IDF build puts every mbedtls allocation into internal RAM
// (CONFIG_MBEDTLS_INTERNAL_MEM_ALLOC), and one TLS connection of the web interface (web_server.cpp) holds two 16 KB
// record buffers: three of them would take most of the ~120 KB internal RAM that AirPlay and Wi-Fi need. These two
// functions replace the core's own (the only ones in libmbedcrypto's esp_mem.c; the linker takes the sketch's
// first): blocks of 4 KB and more (record buffers, big numbers' work areas) go to PSRAM, everything smaller stays
// internal, as before. ESP-IDF supports mbedtls in PSRAM (CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC): the AES / SHA
// accelerators copy external buffers through internal ones.
#include <esp_heap_caps.h>
#include <stddef.h>

#define TLS_PSRAM_FROM 4096

void *esp_mbedtls_mem_calloc(size_t n, size_t size) {
  if (size && n > (size_t)-1 / size) return NULL;
  if (n * size >= TLS_PSRAM_FROM) {
    void *p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p) return p;
  }
  return heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

void esp_mbedtls_mem_free(void *p) { heap_caps_free(p); }
