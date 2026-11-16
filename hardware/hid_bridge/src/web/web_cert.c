// The web interface's certificate: see web_cert.h.
#include "web_cert.h"
#include <stdio.h>
#include <string.h>
#include "mbedtls/ecp.h"
#include "mbedtls/oid.h"
#include "mbedtls/pk.h"
#include "mbedtls/sha256.h"
#include "mbedtls/x509_crt.h"

// Howard Hinnant's days_from_civil / civil_from_days.
int32_t wc_days(int y, int m, int d) {
  y -= m <= 2;
  int32_t era = (y >= 0 ? y : y - 399) / 400;
  int32_t yoe = y - era * 400;
  int32_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  int32_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + doe - 719468;
}

void wc_date(int32_t z, int *y, int *m, int *d) {
  z += 719468;
  int32_t era = (z >= 0 ? z : z - 146096) / 146097;
  int32_t doe = z - era * 146097;
  int32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  int32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  int32_t mp = (5 * doy + 2) / 153;
  *d = doy - (153 * mp + 2) / 5 + 1;
  *m = mp < 10 ? mp + 3 : mp - 9;
  *y = yoe + era * 400 + (*m <= 2);
}

int32_t wc_build_days(const char *date) {
  static const char *months = "JanFebMarAprMayJunJulAugSepOctNovDec";
  char mon[4] = {0};
  int d, y;
  if (!date || sscanf(date, "%3s %d %d", mon, &d, &y) != 3) return -1;
  const char *at = strstr(months, mon);
  if (!at || (at - months) % 3 || d < 1 || d > 31 || y < 2000) return -1;
  return wc_days(y, (int)(at - months) / 3 + 1, d);
}

static void stamp(int32_t days, char out[15]) {
  int y, m, d;
  wc_date(days, &y, &m, &d);
  snprintf(out, 15, "%04d%02d%02d000000", y, m, d);
}

int wc_make(const char *dns, uint32_t ipv4, int32_t from_days, wc_rng rng, void *rng_ctx, char key_pem[WC_KEY_PEM_MAX],
            char crt_pem[WC_CRT_PEM_MAX]) {
  mbedtls_pk_context key;
  mbedtls_x509write_cert crt;
  mbedtls_pk_init(&key);
  mbedtls_x509write_crt_init(&crt);
  int r = mbedtls_pk_setup(&key, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
  if (!r) r = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(key), rng, rng_ctx);
  if (!r) r = mbedtls_pk_write_key_pem(&key, (unsigned char *)key_pem, WC_KEY_PEM_MAX);
  if (r) goto done;

  char name[96], from[15], until[15];
  snprintf(name, sizeof name, "CN=%s,O=Tentacle", dns);
  stamp(from_days, from);
  stamp(from_days + WC_DAYS, until);
  unsigned char serial[16];
  if ((r = rng(rng_ctx, serial, sizeof serial))) goto done;
  serial[0] = (serial[0] & 0x7f) | 0x40;  // positive, and 16 bytes long

  mbedtls_x509write_crt_set_version(&crt, MBEDTLS_X509_CRT_VERSION_3);
  mbedtls_x509write_crt_set_md_alg(&crt, MBEDTLS_MD_SHA256);
  mbedtls_x509write_crt_set_subject_key(&crt, &key);
  mbedtls_x509write_crt_set_issuer_key(&crt, &key);
  if ((r = mbedtls_x509write_crt_set_subject_name(&crt, name)) || (r = mbedtls_x509write_crt_set_issuer_name(&crt, name)) ||
      (r = mbedtls_x509write_crt_set_serial_raw(&crt, serial, sizeof serial)) ||
      (r = mbedtls_x509write_crt_set_validity(&crt, from, until)) ||
      (r = mbedtls_x509write_crt_set_basic_constraints(&crt, 0, -1)) ||
      (r = mbedtls_x509write_crt_set_key_usage(&crt, MBEDTLS_X509_KU_DIGITAL_SIGNATURE)))
    goto done;

  mbedtls_asn1_sequence eku = {0};
  eku.buf.tag = MBEDTLS_ASN1_OID;
  eku.buf.p = (unsigned char *)MBEDTLS_OID_SERVER_AUTH;
  eku.buf.len = MBEDTLS_OID_SIZE(MBEDTLS_OID_SERVER_AUTH);
  if ((r = mbedtls_x509write_crt_set_ext_key_usage(&crt, &eku))) goto done;

  // Subject alternative names: browsers match only these, never the CN.
  unsigned char ip[4] = {(unsigned char)ipv4, (unsigned char)(ipv4 >> 8), (unsigned char)(ipv4 >> 16),
                         (unsigned char)(ipv4 >> 24)};
  mbedtls_x509_san_list san_ip = {0}, san_dns = {0};
  san_dns.node.type = MBEDTLS_X509_SAN_DNS_NAME;
  san_dns.node.san.unstructured_name.p = (unsigned char *)dns;
  san_dns.node.san.unstructured_name.len = strlen(dns);
  if (ipv4) {
    san_ip.node.type = MBEDTLS_X509_SAN_IP_ADDRESS;
    san_ip.node.san.unstructured_name.p = ip;
    san_ip.node.san.unstructured_name.len = 4;
    san_dns.next = &san_ip;
  }
  if ((r = mbedtls_x509write_crt_set_subject_alternative_name(&crt, &san_dns))) goto done;
  r = mbedtls_x509write_crt_pem(&crt, (unsigned char *)crt_pem, WC_CRT_PEM_MAX, rng, rng_ctx);

done:
  mbedtls_x509write_crt_free(&crt);
  mbedtls_pk_free(&key);
  if (r) key_pem[0] = crt_pem[0] = 0;
  return r;
}

bool wc_info(const char *crt_pem, char fp[WC_FP_LEN], int32_t *until_days) {
  if (!crt_pem || !crt_pem[0]) return false;
  mbedtls_x509_crt c;
  mbedtls_x509_crt_init(&c);
  bool ok = !mbedtls_x509_crt_parse(&c, (const unsigned char *)crt_pem, strlen(crt_pem) + 1);
  if (ok) {
    if (fp) {
      unsigned char sum[32];
      mbedtls_sha256(c.raw.p, c.raw.len, sum, 0);
      for (int i = 0; i < 32; i++) snprintf(fp + 3 * i, 4, i < 31 ? "%02X:" : "%02X", sum[i]);
    }
    if (until_days) *until_days = wc_days(c.valid_to.year, c.valid_to.mon, c.valid_to.day);
  }
  mbedtls_x509_crt_free(&c);
  return ok;
}

bool wc_good(const char *crt_pem, int32_t build_days) {
  int32_t until;
  return wc_info(crt_pem, NULL, &until) && until - build_days > WC_RENEW_DAYS;
}
