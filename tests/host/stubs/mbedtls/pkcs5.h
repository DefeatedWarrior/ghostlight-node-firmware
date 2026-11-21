/* Host stub: a deterministic stand-in for PBKDF2 (NOT a real KDF), enough
 * to test that config_manager stores and checks the admin password. */
#pragma once
#include <stddef.h>
#include <stdint.h>
typedef enum { MBEDTLS_MD_NONE = 0, MBEDTLS_MD_SHA256 = 9 } mbedtls_md_type_t;
int mbedtls_pkcs5_pbkdf2_hmac_ext(mbedtls_md_type_t md_type,
                                  const unsigned char *password, size_t plen,
                                  const unsigned char *salt, size_t slen,
                                  unsigned int iteration_count,
                                  uint32_t key_length, unsigned char *output);
