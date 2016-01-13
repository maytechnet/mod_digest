/*
 * ProFTPD: mod_digest - File hashing/checksumming module
 *
 * Copyright (c) Mathias Berchtold <mb@smartftp.com>
 * Copyright (c) 2016 TJ Saunders <tj@castaglia.org>
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Suite 500, Boston, MA 02110-1335, USA.
 *
 * As a special exemption, TJ Saunders and other respective copyright holders
 * give permission to link this program with OpenSSL, and distribute the
 * resulting executable, without including the source code for OpenSSL in the
 * source distribution.
 *
 * -----DO NOT EDIT BELOW THIS LINE-----
 * $Libraries: -lcrypto$
 */

#include "conf.h"

#define MOD_DIGEST_VERSION      "mod_digest/2.0.0"

/* Define the custom commands/responses used. */
#ifndef C_HASH
# define C_HASH		"HASH"
#endif
#ifndef C_XCRC
# define C_XCRC		"XCRC"
#endif
#ifndef C_XMD5
# define C_XMD5		"XMD5"
#endif
#ifndef C_XSHA
# define C_XSHA		"XSHA"
#endif
#ifndef C_XSHA1
# define C_XSHA1	"XSHA1"
#endif
#ifndef C_XSHA256
# define C_XSHA256	"XSHA256"
#endif
#ifndef C_XSHA512
# define C_XSHA512	"XSHA512"
#endif

#ifndef R_556
# define R_556		"556"
#endif

/* Make sure the version of proftpd is as necessary. */
#if PROFTPD_VERSION_NUMBER < 0x0001030304
# error "ProFTPD 1.3.3 or later required"
#endif

#if !defined(HAVE_OPENSSL) && !defined(PR_USE_OPENSSL)
# error "OpenSSL support required (--enable-openssl)"
#else
# include <openssl/bio.h>
# include <openssl/evp.h>
# include <openssl/err.h>
#endif

module digest_module;

static int digest_caching = TRUE;

#ifndef DIGEST_CACHE_DEFAULT_SIZE
# define DIGEST_CACHE_DEFAULT_SIZE	10000
#endif
static size_t digest_cache_max_size = DIGEST_CACHE_DEFAULT_SIZE;

static EVP_MD_CTX *digest_cache_xfer_ctx = NULL;

static int digest_engine = TRUE;
static pool *digest_pool = NULL;

#define DIGEST_OPT_NO_TRANSFER_CACHE		0x0001

/* Note that the internal APIs for opportunistic caching only appeared,
 * in working order, in 1.3.6rc2.  So disable it by default for earlier
 * versions of proftpd.
 */
#if PROFTPD_VERSION_NUMBER < 0x0001030602
# define DIGEST_DEFAULT_OPTS			DIGEST_OPT_NO_TRANSFER
#else
# define DIGEST_DEFAULT_OPTS			0UL
#endif

static unsigned long digest_opts = DIGEST_DEFAULT_OPTS;

/* Tables used as in-memory caches. */
static pr_table_t *digest_crc32_tab = NULL;
static pr_table_t *digest_md5_tab = NULL;
static pr_table_t *digest_sha1_tab = NULL;
static pr_table_t *digest_sha256_tab = NULL;
static pr_table_t *digest_sha512_tab = NULL;

/* Digest algorithms supported by mod_digest. */
#define DIGEST_ALGO_CRC32		0x0001
#ifndef OPENSSL_NO_MD5
# define DIGEST_ALGO_MD5		0x0002
#else
# define DIGEST_ALGO_MD5		0x0000
#endif /* OPENSSL_NO_MD5 */
#ifndef OPENSSL_NO_SHA
# define DIGEST_ALGO_SHA1		0x0004
#else
# define DIGEST_ALGO_SHA1		0x0000
#endif /* OPENSSL_NO_SHA */
#ifndef OPENSSL_NO_SHA256
# define DIGEST_ALGO_SHA256		0x0008
#else
# define DIGEST_ALGO_SHA256		0x0000
#endif /* OPENSSL_NO_SHA256 */
#ifndef OPENSSL_NO_SHA512
# define DIGEST_ALGO_SHA512		0x0010
#else
# define DIGEST_ALGO_SHA512		0x0000
#endif /* OPENSSL_NO_SHA512 */

#define DIGEST_DEFAULT_ALGOS \
  (DIGEST_ALGO_CRC32|DIGEST_ALGO_MD5|DIGEST_ALGO_SHA1|DIGEST_ALGO_SHA256|DIGEST_ALGO_SHA512)

static unsigned long digest_algos = DIGEST_DEFAULT_ALGOS;

static const EVP_MD *digest_hash_md = NULL;
static unsigned long digest_hash_algo = DIGEST_ALGO_SHA1;

/* Flags for determining the style of hash function names. */
#define DIGEST_ALGO_FL_IANA_STYLE	0x0001

/* We will invoke the progress callback every Nth iteration of the read(2)
 * loop when digesting a file.
 */
#ifndef DIGEST_PROGRESS_NTH_ITER
# define DIGEST_PROGRESS_NTH_ITER	10000
#endif

static const char *trace_channel = "digest";

/* Necessary prototypes. */
static void digest_data_xfer_ev(const void *event_data, void *user_data);
static const char *get_algo_name(unsigned long algo, int flags);

#if PROFTPD_VERSION_NUMBER < 0x0001030602
# define PR_STR_FL_HEX_USE_UC			0x0001
# define PR_STR_FL_HEX_USE_LC			0x0002
# define pr_str_bin2hex         		digest_bin2hex

static char *digest_bin2hex(pool *p, const unsigned char *buf, size_len,
    int flags) {
  static const char *hex_lc = "0123456789abcdef", *hex_uc = "0123456789ABCDEF";
  register unsigned int i;
  const char *hex_vals;
  char *hex, *ptr;
  size_t hex_len;

  if (p == NULL ||
      buf == NULL) {
    errno = EINVAL;
    return NULL;
  }

  if (len == 0) {
    return pstrdup(p, "");
  }

  /* By default, we use lowercase hex values. */
  hex_vals = hex_lc;
  if (flags & PR_STR_FL_HEX_USE_UC) {
    hex_vals = hex_uc;
  }


  hex_len = (len * 2) + 1;
  hex = palloc(p, hex_len);

  ptr = hex;
  for (i = 0; i < len; i++) {
    *ptr++ = hex_vals[buf[i] >> 4];
    *ptr++ = hex_vals[buf[i] % 16];
  }
  *ptr = '\0';

  return hex;
}
#endif

/* CRC32 implementation, as OpenSSL EVP_MD.  The following OpenSSL files
 * used as templates:
 *
 *  crypto/evp/m_md2.c
 *  crypto/md2/md2.c
 */

#define CRC32_BLOCK		4
#define CRC32_DIGEST_LENGTH	4

typedef struct crc32_ctx_st {
  uint32_t crc32_table[256];
  uint32_t data;
} CRC32_CTX;

static int CRC32_Init(CRC32_CTX *ctx) {
  register unsigned int i;

  /* Initialize the lookup table.   The magic number in the loop is the official
   * polynomial used by CRC32 in PKZip.
   */

  for (i = 0; i < sizeof(ctx->crc32_table); i++) {
    register unsigned int j;
    uint32_t crc;

    crc = i;
    for (j = 8; j > 0; j--) {
      if (crc & 1) {
        crc = (crc >> 1) ^ 0xEDB88320;
      } else {
        crc >>= 1;
      }
    }

    ctx->crc32_table[i] = crc;
  }

  ctx->data = 0xffffffff;
  return 1;
}

#define CRC32(ctx, c, b) (ctx->crc32_table[((int)(c) ^ (b)) & 0xff] ^ ((c) >> 8))
#define DOCRC(ctx, c, d)  c = CRC32(ctx, c, *d++)

static int CRC32_Update(CRC32_CTX *ctx, const unsigned char *data,
    size_t datasz) {

  if (datasz == 0) {
    return 1;
  }

  while (datasz > 0) {
    DOCRC(ctx, ctx->data, data);
    datasz--;
  }

  return 1;
}

static int CRC32_Final(unsigned char *md, CRC32_CTX *ctx) {
  uint32_t crc;

  crc = ctx->data;
  crc ^= 0xffffffff;
  crc = htonl(crc);

  memcpy(md, &crc, sizeof(crc));
  return 1;
}

static int crc32_init(EVP_MD_CTX *ctx) {
  return CRC32_Init(ctx->md_data);
}

static int crc32_update(EVP_MD_CTX *ctx, const void *data, size_t datasz) {
  return CRC32_Update(ctx->md_data, data, datasz);
}

static int crc32_final(EVP_MD_CTX *ctx, unsigned char *md) {
  return CRC32_Final(md, ctx->md_data);
}

static const EVP_MD crc32_md = {
  NID_undef,
  NID_undef,
  CRC32_DIGEST_LENGTH,
  0,
  crc32_init,
  crc32_update,
  crc32_final,
  NULL,
  NULL,
  EVP_PKEY_NULL_method,
  CRC32_BLOCK,
  sizeof(EVP_MD *) + sizeof(CRC32_CTX)
};

static const EVP_MD *EVP_crc32(void) {
  return &crc32_md;
}

static const char *get_errors(void) {
  unsigned int count = 0;
  unsigned long error_code;
  BIO *bio = NULL;
  char *data = NULL;
  long datalen;
  const char *error_data = NULL, *str = "(unknown)";
  int error_flags = 0;

  /* Use ERR_print_errors() and a memory BIO to build up a string with
   * all of the error messages from the error queue.
   */

  error_code = ERR_get_error_line_data(NULL, NULL, &error_data, &error_flags);
  if (error_code) {
    bio = BIO_new(BIO_s_mem());
  }

  while (error_code) {
    pr_signals_handle();

    if (error_flags & ERR_TXT_STRING) {
      BIO_printf(bio, "\n  (%u) %s [%s]", ++count,
        ERR_error_string(error_code, NULL), error_data);

    } else {
      BIO_printf(bio, "\n  (%u) %s", ++count,
        ERR_error_string(error_code, NULL));
    }

    error_data = NULL;
    error_flags = 0;
    error_code = ERR_get_error_line_data(NULL, NULL, &error_data, &error_flags);
  }

  datalen = BIO_get_mem_data(bio, &data);
  if (data) {
    data[datalen] = '\0';
    str = pstrdup(session.pool, data);
  }
  if (bio != NULL) {
    BIO_free(bio);
  }

  return str;
}

static void digest_hash_feat_add(pool *p) {
  char *feat_str = "";
  int flags;

  /* Per Draft, the hash function names should be those used in:
   *  https://www.iana.org/assignments/hash-function-text-names/hash-function-text-names.txt
   */
  flags = DIGEST_ALGO_FL_IANA_STYLE;

  if (digest_algos & DIGEST_ALGO_CRC32) {
    int current_hash;

    current_hash = (digest_hash_algo == DIGEST_ALGO_CRC32);
    feat_str = pstrcat(p, *feat_str ? feat_str : "",
      get_algo_name(DIGEST_ALGO_CRC32, flags), current_hash ? "*" : "", ";",
      NULL);
  }

  if (digest_algos & DIGEST_ALGO_MD5) {
    int current_hash;

    current_hash = (digest_hash_algo == DIGEST_ALGO_MD5);
    feat_str = pstrcat(p, *feat_str ? feat_str : "",
      get_algo_name(DIGEST_ALGO_MD5, flags), current_hash ? "*" : "", ";",
      NULL);
  }

  if (digest_algos & DIGEST_ALGO_SHA1) {
    int current_hash;

    current_hash = (digest_hash_algo == DIGEST_ALGO_SHA1);
    feat_str = pstrcat(p, *feat_str ? feat_str : "",
      get_algo_name(DIGEST_ALGO_SHA1, flags), current_hash ? "*" : "", ";",
      NULL);
  }

  if (digest_algos & DIGEST_ALGO_SHA256) {
    int current_hash;

    current_hash = (digest_hash_algo == DIGEST_ALGO_SHA256);
    feat_str = pstrcat(p, *feat_str ? feat_str : "",
      get_algo_name(DIGEST_ALGO_SHA256, flags), current_hash ? "*" : "", ";",
      NULL);
  }

  if (digest_algos & DIGEST_ALGO_SHA512) {
    int current_hash;

    current_hash = (digest_hash_algo == DIGEST_ALGO_SHA512);
    feat_str = pstrcat(p, *feat_str ? feat_str : "",
      get_algo_name(DIGEST_ALGO_SHA512, flags), current_hash ? "*" : "", ";",
      NULL);
  }

  feat_str = pstrcat(p, "HASH ", feat_str, NULL);
  pr_feat_add(feat_str);
}

static void digest_hash_feat_remove(void) {
  const char *feat, *hash_feat = NULL;

  feat = pr_feat_get();
  while (feat != NULL) {
    pr_signals_handle();

    if (strncmp(feat, C_HASH, 4) == 0) {
      hash_feat = feat;
      break;
    }

    feat = pr_feat_get_next();
  }

  if (hash_feat != NULL) {
    pr_feat_remove(hash_feat);
  }
}

static void digest_x_feat_add(pool *p) {
  if (digest_algos & DIGEST_ALGO_CRC32) {
    pr_feat_add(C_XCRC);
  }

  if (digest_algos & DIGEST_ALGO_MD5) {
    pr_feat_add(C_XMD5);
  }

  if (digest_algos & DIGEST_ALGO_SHA1) {
    pr_feat_add(C_XSHA);
    pr_feat_add(C_XSHA1);
  }

  if (digest_algos & DIGEST_ALGO_SHA256) {
    pr_feat_add(C_XSHA256);
  }

  if (digest_algos & DIGEST_ALGO_SHA512) {
    pr_feat_add(C_XSHA512);
  }
}

static void digest_x_help_add(pool *p) {
  if (digest_algos & DIGEST_ALGO_CRC32) {
    pr_help_add(C_XCRC, _("<sp> pathname [<sp> start <sp> end]"), TRUE);
  }

  if (digest_algos & DIGEST_ALGO_MD5) {
    pr_help_add(C_XMD5, _("<sp> pathname [<sp> start <sp> end]"), TRUE);
  }

  if (digest_algos & DIGEST_ALGO_SHA1) {
    pr_help_add(C_XSHA, _("<sp> pathname [<sp> start <sp> end]"), TRUE);
    pr_help_add(C_XSHA1, _("<sp> pathname [<sp> start <sp> end]"), TRUE);
  }

  if (digest_algos & DIGEST_ALGO_SHA256) {
    pr_help_add(C_XSHA256, _("<sp> pathname [<sp> start <sp> end]"), TRUE);
  }

  if (digest_algos & DIGEST_ALGO_SHA512) {
    pr_help_add(C_XSHA512, _("<sp> pathname [<sp> start <sp> end]"), TRUE);
  }
}

/* Configuration handlers
 */

/* Usage: DigestAlgorithms algo1 ... */
MODRET set_digestalgorithms(cmd_rec *cmd) {
  config_rec *c;
  unsigned long algos = 0UL;

  CHECK_CONF(cmd, CONF_ROOT|CONF_GLOBAL|CONF_VIRTUAL|CONF_ANON);

  /* We need at least ONE algorithm. */
  if (cmd->argc < 2) {
    CONF_ERROR(cmd, "wrong number of parameters");
  }

  if (strcasecmp(cmd->argv[1], "all") == 0) {
    algos = DIGEST_DEFAULT_ALGOS;

  } else {
    register unsigned int i;

    for (i = 1; i < cmd->argc; i++) {
      if (strcasecmp(cmd->argv[i], "crc32") == 0) {
        algos |= DIGEST_ALGO_CRC32;

      } else if (strcasecmp(cmd->argv[i], "md5") == 0) {
#ifndef OPENSSL_NO_MD5
        algos |= DIGEST_ALGO_MD5;
#else
        CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "installed OpenSSL does not support the '", cmd->argv[i], "' DigestAlgorithm", NULL));
#endif /* OPENSSL_NO_MD5 */

      } else if (strcasecmp(cmd->argv[i], "sha1") == 0) {
#ifndef OPENSSL_NO_SHA
        algos |= DIGEST_ALGO_SHA1;
#else
        CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "installed OpenSSL does not support the '", cmd->argv[i], "' DigestAlgorithm", NULL));
#endif /* OPENSSL_NO_SHA */

      } else if (strcasecmp(cmd->argv[i], "sha256") == 0) {
#ifndef OPENSSL_NO_SHA256
        algos |= DIGEST_ALGO_SHA256;
#else
        CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "installed OpenSSL does not support the '", cmd->argv[i], "' DigestAlgorithm", NULL));
#endif /* OPENSSL_NO_SHA256 */

      } else if (strcasecmp(cmd->argv[i], "sha512") == 0) {
#ifndef OPENSSL_NO_SHA512
        algos |= DIGEST_ALGO_SHA512;
#else
        CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "installed OpenSSL does not support the '", cmd->argv[i], "' DigestAlgorithm", NULL));
#endif /* OPENSSL_NO_SHA512 */

      } else {
        CONF_ERROR(cmd, pstrcat(cmd->tmp_pool,
          "unknown/unsupported DigestAlgorithm: ", cmd->argv[i], NULL));
      }
    }
  }

  c = add_config_param(cmd->argv[0], 1, NULL);
  c->argv[0] = palloc(c->pool, sizeof(unsigned long));
  *((unsigned long *) c->argv[0]) = algos;
  c->flags |= CF_MERGEDOWN;

  return PR_HANDLED(cmd);
}

/* usage: DigestCache on|off|size */
MODRET set_digestcache(cmd_rec *cmd) {
  config_rec *c;
  int caching = -1;
  size_t max_size = DIGEST_CACHE_DEFAULT_SIZE;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_GLOBAL|CONF_VIRTUAL|CONF_ANON);

  caching = get_boolean(cmd, 1);
  if (caching == -1) {
    long size;
    char *ptr = NULL;

    size = strtol(cmd->argv[1], &ptr, 10);
    if (ptr && *ptr) {
      CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "invalid cache size: ",
        cmd->argv[1], NULL));
    }

    if (size < 1) {
      CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "cache size must be > 0"));
    }

    caching = TRUE;
    max_size = size;
  }

  c = add_config_param(cmd->argv[0], 2, NULL, NULL);
  c->argv[0] = palloc(c->pool, sizeof(int));
  *((int *) c->argv[0]) = caching;
  c->argv[1] = palloc(c->pool, sizeof(size_t));
  *((size_t *) c->argv[1]) = max_size;

  c->flags |= CF_MERGEDOWN;
  return PR_HANDLED(cmd);
}

/* usage: DigestEngine on|off */
MODRET set_digestengine(cmd_rec *cmd) {
  int engine = -1;
  config_rec *c;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL|CONF_ANON);

  c = add_config_param(cmd->argv[0], 1, NULL);
  c->argv[0] = palloc(c->pool, sizeof(int));
  *((int *) c->argv[0]) = engine;

  c->flags |= CF_MERGEDOWN;
  return PR_HANDLED(cmd);
}

/* usage: DigestMaxSize len */
MODRET set_digestmaxsize(cmd_rec *cmd) {
  config_rec *c = NULL;
  char *ptr = NULL;
  off_t max_size;

  CHECK_ARGS(cmd, 1);
  CHECK_CONF(cmd, CONF_ROOT|CONF_GLOBAL|CONF_VIRTUAL|CONF_ANON);

#ifdef HAVE_STRTOULL
  max_size = strtoull(cmd->argv[1], &ptr, 10);
#else
  max_size = strtoul(cmd->argv[1], &ptr, 10);
#endif /* HAVE_STRTOULL */

  if (ptr && *ptr) {
    CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, "badly formatted size value: ",
      cmd->argv[1], NULL));
  }

  if (max_size == 0) {
    CONF_ERROR(cmd, "requires a value greater than zero");
  }

  c = add_config_param(cmd->argv[0], 1, NULL);
  c->argv[0] = pcalloc(c->pool, sizeof(off_t));
  *((off_t *) c->argv[0]) = max_size;
  c->flags |= CF_MERGEDOWN;

  return PR_HANDLED(cmd);
}

/* usage: DigestOptions opt1 ... */
MODRET set_digestoptions(cmd_rec *cmd) {
  config_rec *c = NULL;
  register unsigned int i = 0;
  unsigned long opts = 0UL;

  if (cmd->argc-1 == 0) {
    CONF_ERROR(cmd, "wrong number of parameters");
  }

  CHECK_CONF(cmd, CONF_ROOT|CONF_VIRTUAL|CONF_GLOBAL);

  c = add_config_param(cmd->argv[0], 1, NULL);

  for (i = 1; i < cmd->argc; i++) {
    if (strcmp(cmd->argv[i], "NoTransferCache") == 0) {
      opts |= DIGEST_OPT_NO_TRANSFER_CACHE;

    } else {
      CONF_ERROR(cmd, pstrcat(cmd->tmp_pool, ": unknown DigestOption '",
        cmd->argv[i], "'", NULL));
    }
  }

  c->argv[0] = pcalloc(c->pool, sizeof(unsigned long));
  *((unsigned long *) c->argv[0]) = opts;

  return PR_HANDLED(cmd);
}

/* returns 1 if enabled. 0 otherwise */
static int digest_isenabled(unsigned long algo) {
  if (digest_algos & algo) {
    return TRUE;
  }

  return FALSE;
}

static int check_digest_max_size(off_t len) {
  config_rec *c;
  off_t max_size;

  c = find_config(CURRENT_CONF, CONF_PARAM, "DigestMaxSize", FALSE);
  if (c == NULL) {
    return 0;
  }

  max_size = *((off_t *) c->argv[0]);

  if (len > max_size) {
    pr_log_debug(DEBUG5, MOD_DIGEST_VERSION
      ": %s requested len (%" PR_LU ") exceeds DigestMaxSize %" PR_LU
      ", rejecting", session.curr_cmd, (pr_off_t) len, (pr_off_t) max_size);
    errno = EPERM;
    return -1;
  }

  return 0;
}

static int check_file(pool *p, const char *path, off_t start, size_t len,
    struct stat *st) {

  if (!S_ISREG(st->st_mode)) {
    pr_trace_msg(trace_channel, 2, "path '%s' is not a regular file", path);
    errno = EISDIR;
    return -1;
  }

  if (start > 0) {
    if (start > st->st_size) {
      pr_log_debug(DEBUG3, MOD_DIGEST_VERSION
        ": requested offset (%" PR_LU " bytes) for path '%s' exceeds file size "
        "(%lu bytes)", (pr_off_t) start, path, (unsigned long) st->st_size);
      errno = EINVAL;
      return -1;
    }
  }

  if (len > 0) {
    if (start + len > st->st_size) {
      pr_log_debug(DEBUG3, MOD_DIGEST_VERSION
        ": requested offset/length (offset %" PR_LU " bytes, length %lu bytes) "
        "for path '%s' exceeds file size (%lu bytes)", (pr_off_t) start,
        (unsigned long) len, path, (unsigned long) st->st_size);
      errno = EINVAL;
      return -1;
    }
  }

  return 0;
}

/* Note that this is implemented in a case-INSENSITIVE manner, in order to
 * protect any unfortunate case-insensitive filesystems (such as HFS on
 * Mac, even though it is case-preserving).
 */
static int blacklisted_file(const char *path) {
  int res = FALSE;

  if (strncasecmp("/dev/full", path, 10) == 0 ||
      strncasecmp("/dev/null", path, 10) == 0 ||
      strncasecmp("/dev/random", path, 12) == 0 ||
      strncasecmp("/dev/urandom", path, 13) == 0 ||
      strncasecmp("/dev/zero", path, 10) == 0) {
    res = TRUE;
  }

  return res;
}

static int compute_digest(pool *p, const char *path, off_t start, off_t len,
    const EVP_MD *md, unsigned char *digest, unsigned int *digest_len,
    time_t *mtime, void (*hash_progress_cb)(const char *, off_t)) {
  int res, xerrno = 0;
  pr_fh_t *fh;
  struct stat st;
  unsigned char *buf;
  size_t bufsz, readsz, iter_count;
  EVP_MD_CTX md_ctx;

  fh = pr_fsio_open(path, O_RDONLY);
  if (fh == NULL) {
    xerrno = errno;

    pr_trace_msg(trace_channel, 1, "unable to read '%s': %s", path,
      strerror(xerrno));

    errno = xerrno;
    return -1;
  }

  res = pr_fsio_fstat(fh, &st);
  if (res < 0) {
    xerrno = errno;

    pr_trace_msg(trace_channel, 1, "unable to stat '%s': %s", path,
      strerror(xerrno));
    (void) pr_fsio_close(fh);

    errno = xerrno;
    return -1;
  }

  res = check_file(p, path, start, len, &st);
  if (res < 0) {
    xerrno = errno;
    (void) pr_fsio_close(fh);
    errno = xerrno;
    return -1;
  }

  if (mtime != NULL) {
    /* Inform the caller of the last-mod-time for this file, for use in
     * e.g caching.
     */
    *mtime = st.st_mtime;
  }

  /* Determine the optimal block size for reading. */
  fh->fh_iosz = bufsz = st.st_blksize;

  if (pr_fsio_lseek(fh, start, SEEK_SET) == (off_t) -1) {
    xerrno = errno;

    pr_trace_msg(trace_channel, 1, "error seeking to offset %" PR_LU
      " in '%s': %s", (pr_off_t) start, path, strerror(xerrno));

    (void) pr_fsio_close(fh);
    errno = xerrno;
    return -1;
  }

  EVP_MD_CTX_init(&md_ctx);
  if (EVP_DigestInit_ex(&md_ctx, md, NULL) != 1) {
    pr_log_debug(DEBUG1, MOD_DIGEST_VERSION
      ": error preparing digest context: %s", get_errors());
    (void) pr_fsio_close(fh);
    errno = EPERM;
    return -1;
  }

  buf = palloc(p, bufsz);

  readsz = bufsz;
  if (readsz > len) {
    readsz = len;
  }

  iter_count = 0;
  res = pr_fsio_read(fh, (char *) buf, readsz);
  xerrno = errno;

  while (len > 0) {
    iter_count++;

    if (res < 0 &&
        errno == EAGAIN) {
      /* Add a small delay by treating this as EINTR. */
      errno = xerrno = EINTR;
    }

    pr_signals_handle();

    if (res < 0 &&
        xerrno == EINTR) {
      /* If we were interrupted, try again. */
      res = pr_fsio_read(fh, (char *) buf, readsz);
      continue;
    }

    if (EVP_DigestUpdate(&md_ctx, buf, res) != 1) {
      pr_log_debug(DEBUG1, MOD_DIGEST_VERSION
        ": error updating digest: %s", get_errors());
    }

    len -= res;

    /* Every Nth iteration, invoke the progress callback. */
    if ((iter_count % DIGEST_PROGRESS_NTH_ITER) == 0) {
      (hash_progress_cb)(path, len);
    }

    readsz = bufsz;
    if (readsz > len) {
      readsz = len;
    }

    res = pr_fsio_read(fh, (char *) buf, readsz);
    xerrno = errno;
  }

  (void) pr_fsio_close(fh);

  if (len != 0) {
    pr_log_debug(DEBUG3, MOD_DIGEST_VERSION
      ": failed to read all %" PR_LU " bytes of '%s' (premature EOF?)",
      (pr_off_t) len, path);
    errno = EIO;
    return -1;
  }

  if (EVP_DigestFinal_ex(&md_ctx, digest, digest_len) != 1) {
    pr_log_debug(DEBUG1, MOD_DIGEST_VERSION
      ": error finishing digest: %s", get_errors());
    errno = EPERM;
    return -1;
  }

  return 0;
}

static const EVP_MD *get_algo_md(unsigned long algo) {
  const EVP_MD *md = NULL;

  switch (algo) {
    case DIGEST_ALGO_CRC32:
      md = EVP_crc32();
      break;

#ifndef OPENSSL_NO_MD5
    case DIGEST_ALGO_MD5:
      md = EVP_md5();
      break;
#endif /* OPENSSL_NO_MD5 */

#ifndef OPENSSL_NO_SHA1
    case DIGEST_ALGO_SHA1:
      md = EVP_sha1();
      break;
#endif /* OPENSSL_NO_SHA1 */

#ifndef OPENSSL_NO_SHA256
    case DIGEST_ALGO_SHA256:
      md = EVP_sha256();
      break;
#endif /* OPENSSL_NO_SHA256 */

#ifndef OPENSSL_NO_SHA512
    case DIGEST_ALGO_SHA512:
      md = EVP_sha512();
      break;
#endif /* OPENSSL_NO_SHA512 */

    default:
      errno = ENOENT;
      break;
  }

  return md;
}

static const char *get_algo_name(unsigned long algo, int flags) {
  const char *algo_name = "(unknown)";

  switch (algo) {
    case DIGEST_ALGO_CRC32:
      algo_name = "CRC32";
      break;

    case DIGEST_ALGO_MD5:
      algo_name = "MD5";
      break;

    case DIGEST_ALGO_SHA1:
      if (flags & DIGEST_ALGO_FL_IANA_STYLE) {
        algo_name = "SHA-1";

      } else {
        algo_name = "SHA1";
      }
      break;

    case DIGEST_ALGO_SHA256:
      if (flags & DIGEST_ALGO_FL_IANA_STYLE) {
        algo_name = "SHA-256";

      } else {
        algo_name = "SHA256";
      }
      break;

    case DIGEST_ALGO_SHA512:
      if (flags & DIGEST_ALGO_FL_IANA_STYLE) {
        algo_name = "SHA-512";

      } else {
        algo_name = "SHA512";
      }
      break;

    default:
      errno = ENOENT;
      break;
  }

  return algo_name;
}

static pr_table_t *get_cache(unsigned long algo) {
  pr_table_t *cache = NULL;

  switch (algo) {
    case DIGEST_ALGO_CRC32:
      cache = digest_crc32_tab;
      break;

    case DIGEST_ALGO_MD5:
      cache = digest_md5_tab;
      break;

    case DIGEST_ALGO_SHA1:
      cache = digest_sha1_tab;
      break;

    case DIGEST_ALGO_SHA256:
      cache = digest_sha256_tab;
      break;

    case DIGEST_ALGO_SHA512:
      cache = digest_sha512_tab;
      break;

    default:
      pr_trace_msg(trace_channel, 4,
        "unable to determine cache for %s digest", get_algo_name(algo, 0));
      errno = EINVAL;
      return NULL;
  }

  if (cache == NULL) {
    errno = ENOENT;
  }

  return cache;
}

static size_t get_cache_size(void) {
  int res;
  size_t cache_size = 0;

  if (digest_caching == FALSE) {
    return 0;
  }

  res = pr_table_count(digest_crc32_tab);
  if (res >= 0) {
    cache_size += res;
  }

  res = pr_table_count(digest_md5_tab);
  if (res >= 0) {
    cache_size += res;
  }

  res = pr_table_count(digest_sha1_tab);
  if (res >= 0) {
    cache_size += res;
  }

  res = pr_table_count(digest_sha256_tab);
  if (res >= 0) {
    cache_size += res;
  }

  res = pr_table_count(digest_sha512_tab);
  if (res >= 0) {
    cache_size += res;
  }

  return cache_size;
}

/* Format the keys for the in-memory caches as:
 *  "<path>@<mtime>,<start>+<len>"
 */
static const char *get_cache_key(pool *p, const char *path,
    time_t mtime, off_t start, size_t len) {
  const char *key;
  char mtime_str[256], start_str[256], len_str[256];

  memset(mtime_str, '\0', sizeof(mtime_str));
  snprintf(mtime_str, sizeof(mtime_str)-1, "%llu", (unsigned long long) mtime);

  memset(start_str, '\0', sizeof(start_str));
  snprintf(start_str, sizeof(start_str)-1, "%" PR_LU, (pr_off_t) start);

  memset(len_str, '\0', sizeof(len_str));
  snprintf(len_str, sizeof(len_str)-1, "%llu", (unsigned long long) len);

  key = pstrcat(p, path, "@", mtime_str, ",", start_str, "+", len_str, NULL);
  return key;
}

static char *get_cached_digest(pool *p, unsigned long algo, const char *path,
    time_t mtime, off_t start, size_t len) {
  const char *key;
  pr_table_t *cache;
  void *val;
  size_t valsz = 0;

  if (digest_caching == FALSE) {
    errno = ENOENT;
    return NULL;
  }

  cache = get_cache(algo);
  if (cache == NULL) {
    return NULL;
  }

  key = get_cache_key(p, path, mtime, start, len);
  if (key == NULL) {
    return NULL;
  }

  val = pr_table_get(cache, key, &valsz);
  if (val != NULL) {
    char *hex_digest;

    hex_digest = palloc(p, valsz);
    memcpy(hex_digest, val, valsz);

    pr_trace_msg(trace_channel, 12,
      "using cached digest '%s' for %s digest, key '%s'", hex_digest,
      get_algo_name(algo, 0), key);
    return hex_digest;
  }

  errno = ENOENT;
  return NULL;
}

static int add_cached_digest(pool *p, unsigned long algo, const char *path,
    time_t mtime, off_t start, size_t len, const char *hex_digest) {
  int res;
  const char *key;
  pr_table_t *cache;

  if (digest_caching == FALSE) {
    return 0;
  }

  cache = get_cache(algo);
  if (cache == NULL) {
    return -1;
  }

  key = get_cache_key(p, path, mtime, start, len);
  if (key == NULL) {
    return -1;
  }

  res = pr_table_add_dup(cache, pstrdup(digest_pool, key),
    (void *) hex_digest, 0);
  if (res == 0) {
    pr_trace_msg(trace_channel, 12,
      "cached digest '%s' for %s digest, key '%s'", hex_digest,
      get_algo_name(algo, 0), key);
  }

  return res;
}

static char *get_digest(cmd_rec *cmd, unsigned long algo, const char *path,
    time_t mtime, off_t start, size_t len, int flags,
    void (*hash_progress_cb)(const char *, off_t)) {
  int res;
  const EVP_MD *md;
  unsigned char *digest = NULL;
  unsigned int digest_len;
  char *hex_digest;
  size_t cache_size;

  /* Note: if caching is disabled, this condition will never be true. */
  cache_size = get_cache_size();
  if (cache_size >= digest_cache_max_size) {
    int xerrno = EAGAIN;

#ifdef EBUSY
    /* This errno value may not be available on all platforms, but it is
     * the most appropriate.
     */
    xerrno = EBUSY;
#endif /* EBUSY */

    pr_log_debug(DEBUG5, MOD_DIGEST_VERSION
      ": cache size (%lu) meets/exceeds max cache size (%lu), "
      "refusing %s command", (unsigned long) cache_size,
      (unsigned long) digest_cache_max_size, (char *) cmd->argv[0]);

    errno = xerrno;
    return NULL;
  }

  hex_digest = get_cached_digest(cmd->tmp_pool, algo, path, mtime, start, len);
  if (hex_digest != NULL) {
    if (flags & PR_STR_FL_HEX_USE_UC) {
      register unsigned int i;

      for (i = 0; hex_digest[i]; i++) {
        hex_digest[i] = toupper((int) hex_digest[i]);
      }
    }

    return hex_digest;
  }

  md = get_algo_md(algo);
  digest_len = EVP_MD_size(md);
  digest = palloc(cmd->tmp_pool, digest_len);

  res = compute_digest(cmd->tmp_pool, path, start, len, md, digest,
    &digest_len, &mtime, hash_progress_cb);
  if (res == 0) {
    hex_digest = pr_str_bin2hex(cmd->tmp_pool, digest, digest_len,
      PR_STR_FL_HEX_USE_LC);
  }

  if (add_cached_digest(cmd->tmp_pool, algo, path, mtime, start, len,
      hex_digest) < 0) {
    pr_trace_msg(trace_channel, 8,
      "error caching %s digest for path '%s': %s", get_algo_name(algo, 0),
      path, strerror(errno));
  }

  if (flags & PR_STR_FL_HEX_USE_UC) {
    register unsigned int i;

    for (i = 0; hex_digest[i]; i++) {
      hex_digest[i] = toupper((int) hex_digest[i]);
    }
  }

  return hex_digest;
}

static void digest_progress_cb(const char *path, off_t remaining) {
  pr_trace_msg(trace_channel, 19,
    "%" PR_LU " bytes remaining for digesting of '%s'", (pr_off_t) remaining,
    path);

  /* Make sure to reset the idle timer, to prevent ProFTPD from timing out
   * the session.
   */
  pr_timer_reset(PR_TIMER_IDLE, ANY_MODULE);

  /* AND write something on the control connection, to prevent any middleboxes
   * from timing out the session.
   */
  pr_response_add(R_DUP, _("Calculating..."));
}

static modret_t *digest_xcmd(cmd_rec *cmd, unsigned long algo) {
  char *orig_path, *path;
  struct stat st;

  CHECK_CMD_MIN_ARGS(cmd, 2);

  /* Note: no support for "XCMD path end" because it's implemented differently
   * by other FTP servers, and is ambiguous (is the 'end' number the end, or
   * the start, or...?).
   */
  if (cmd->argc == 3) {
    pr_response_add_err(R_501, _("Invalid number of arguments"));
    return PR_ERROR((cmd));
  }

  /* XXX Watch out for paths with spaces in them! */
  orig_path = cmd->argv[1];
  path = dir_realpath(cmd->tmp_pool, orig_path);
  if (path == NULL) {
    int xerrno = errno;

    pr_response_add_err(R_550, "%s: %s", orig_path, strerror(xerrno));

    pr_cmd_set_errno(cmd, xerrno);
    errno = xerrno;
    return PR_ERROR(cmd);
  }

  if (blacklisted_file(path) == TRUE) {
    pr_log_debug(DEBUG8, MOD_DIGEST_VERSION
      ": rejecting request to checksum blacklisted special file '%s'", path);
    pr_response_add_err(R_550, "%s: %s", (char *) cmd->arg, strerror(EPERM));
    pr_cmd_set_errno(cmd, EPERM);
    errno = EPERM;
    return PR_ERROR(cmd);
  }

  if (!dir_check(cmd->tmp_pool, cmd, cmd->group, path, NULL)) {
    int xerrno = EPERM;

    pr_log_debug(DEBUG8, MOD_DIGEST_VERSION
      ": %s denied by <Limit> configuration", (char *) cmd->argv[0]);
    pr_response_add_err(R_550, "%s: %s", orig_path, strerror(xerrno));

    pr_cmd_set_errno(cmd, xerrno);
    errno = xerrno;
    return PR_ERROR(cmd);
  }

  pr_fs_clear_cache2(path);
  if (pr_fsio_stat(path, &st) < 0) {
    int xerrno = errno;

    pr_response_add_err(R_550, "%s: %s", orig_path, strerror(xerrno));

    pr_cmd_set_errno(cmd, xerrno);
    errno = xerrno;
    return PR_ERROR(cmd);
  }

  if (!S_ISREG(st.st_mode)) {
    pr_log_debug(DEBUG5, MOD_DIGEST_VERSION
      ": unable to handle %s for non-file path '%s'", (char *) cmd->argv[0],
      path);
    pr_response_add_err(R_550, _("%s: Not a regular file"), orig_path);
    return PR_ERROR(cmd);

  } else {
    off_t len, start_pos, end_pos;

    if (cmd->argc > 3) {
      char *ptr = NULL;

#ifdef HAVE_STRTOULL
      start_pos = strtoull(cmd->argv[2], &ptr, 10);
#else
      start_pos = strtoul(cmd->argv[2], &ptr, 10);
#endif /* HAVE_STRTOULL */

      if (ptr && *ptr) {
        pr_response_add_err(R_501,
          _("%s requires a start greater than or equal to 0"),
          (char *) cmd->argv[0]);
        return PR_ERROR(cmd);
      }

      ptr = NULL;
#ifdef HAVE_STRTOULL
      end_pos = strtoull(cmd->argv[3], &ptr, 10);
#else
      end_pos = strtoul(cmd->argv[3], &ptr, 10);
#endif /* HAVE_STRTOULL */

      if (ptr && *ptr) {
        pr_response_add_err(R_501,
          _("%s requires an end greater than 0"), (char *) cmd->argv[0]);
        return PR_ERROR(cmd);
      }

    } else {
      start_pos = 0;
      end_pos = st.st_size;
    }

    len = end_pos - start_pos;

    if (start_pos > end_pos) {
      pr_response_add_err(R_501,
        _("%s requires end (%" PR_LU ") greater than start (%" PR_LU ")"),
        (char *) cmd->argv[0], (pr_off_t) end_pos, (pr_off_t) end_pos);
      return PR_ERROR(cmd);
    }

    if (check_digest_max_size(len) < 0) {
      pr_response_add_err(R_550, "%s: %s", orig_path, strerror(EPERM));
      pr_cmd_set_errno(cmd, EPERM);
      errno = EPERM;
      return PR_ERROR(cmd);
    }

    if (get_algo_md(algo) != NULL) {
      char *hex_digest;

      pr_response_add(R_250, _("%s: Calculating %s digest"),
        (char *) cmd->argv[0], get_algo_name(algo, 0));
      hex_digest = get_digest(cmd, algo, path, st.st_mtime, start_pos, len,
        PR_STR_FL_HEX_USE_UC, digest_progress_cb);
      if (hex_digest != NULL) {
        pr_response_add(R_DUP, "%s", hex_digest);
        return PR_HANDLED(cmd);
      }

      /* TODO: More detailed error message? */
      pr_response_add_err(R_550, "%s: %s", orig_path, strerror(errno));

    } else {
      pr_response_add_err(R_550, _("%s: Hash algorithm not available"),
        (char *) cmd->argv[0]);
    }
  }

  return PR_ERROR(cmd);
}

/* Command handlers
 */

MODRET digest_hash(cmd_rec *cmd) {
  int xerrno = 0;
  char *error_code = NULL, *orig_path = NULL, *path = NULL, *hex_digest = NULL;
  struct stat st;
  off_t len, start_pos, end_pos;

  if (digest_engine == FALSE) {
    return PR_DECLINED(cmd);
  }

  CHECK_CMD_MIN_ARGS(cmd, 2);

  orig_path = pr_fs_decode_path(cmd->tmp_pool, cmd->arg);
  path = dir_realpath(cmd->tmp_pool, orig_path);
  if (path == NULL) {
    xerrno = errno;

    pr_response_add_err(R_550, "%s: %s", orig_path, strerror(xerrno));

    pr_cmd_set_errno(cmd, xerrno);
    errno = xerrno;
    return PR_ERROR(cmd);
  }

  if (blacklisted_file(path) == TRUE) {
    pr_log_debug(DEBUG8, MOD_DIGEST_VERSION
      ": rejecting request to checksum blacklisted special file '%s'", path);
    pr_response_add_err(R_556, "%s: %s", (char *) cmd->arg, strerror(EPERM));
    pr_cmd_set_errno(cmd, EPERM);
    errno = EPERM;
    return PR_ERROR(cmd);
  }

  if (!dir_check(cmd->tmp_pool, cmd, cmd->group, path, NULL)) {
    xerrno = EPERM;

    pr_log_debug(DEBUG8, MOD_DIGEST_VERSION
      ": %s denied by <Limit> configuration", (char *) cmd->argv[0]);
    pr_response_add_err(R_552, "%s: %s", orig_path, strerror(xerrno));

    pr_cmd_set_errno(cmd, xerrno);
    errno = xerrno;
    return PR_ERROR(cmd);
  }

  pr_fs_clear_cache2(path);
  if (pr_fsio_stat(path, &st) < 0) {
    xerrno = errno;

    pr_response_add_err(R_550, "%s: %s", orig_path, strerror(xerrno));

    pr_cmd_set_errno(cmd, xerrno);
    errno = xerrno;
    return PR_ERROR(cmd);
  }

  if (!S_ISREG(st.st_mode)) {
    pr_log_debug(DEBUG5, MOD_DIGEST_VERSION
      ": unable to handle %s for non-file path '%s'", (char *) cmd->argv[0],
      path);
    pr_response_add_err(R_553, _("%s: Not a regular file"), orig_path);
    return PR_ERROR(cmd);
  }

  start_pos = 0;
  end_pos = st.st_size;
  len = end_pos - start_pos;

  if (check_digest_max_size(len) < 0) {
    pr_response_add_err(R_556, "%s: %s", orig_path, strerror(EPERM));
    pr_cmd_set_errno(cmd, EPERM);
    errno = EPERM;
    return PR_ERROR(cmd);
  }

  pr_trace_msg(trace_channel, 14, "%s: using %s algorithm on path '%s'",
    (char *) cmd->argv[0], get_algo_name(digest_hash_algo, 0), path);

  pr_response_add(R_213, _("%s: Calculating %s digest"), (char *) cmd->argv[0],
    get_algo_name(digest_hash_algo, DIGEST_ALGO_FL_IANA_STYLE));
  hex_digest = get_digest(cmd, digest_hash_algo, path, st.st_mtime, start_pos,
    len, PR_STR_FL_HEX_USE_LC, digest_progress_cb);
  xerrno = errno;

  if (hex_digest != NULL) {
    pr_response_add(R_DUP, "%s %" PR_LU "-%" PR_LU " %s %s",
      get_algo_name(digest_hash_algo, DIGEST_ALGO_FL_IANA_STYLE),
      (pr_off_t) start_pos, (pr_off_t) end_pos, hex_digest, orig_path);
    return PR_HANDLED(cmd);
  }

  switch (xerrno) {
#ifdef EBUSY
    case EBUSY:
#endif
    case EAGAIN:
      /* The HASH draft recommends using 450 for these cases. */
      error_code = R_450;
      break;

    case EISDIR:
      /* The HASH draft recommends using 553 for these cases. */
      error_code = R_553;
      break;

    default:
      error_code = R_550;
      break;
  }

  /* TODO: More detailed error message? */
  pr_response_add_err(error_code, "%s: %s", orig_path, strerror(xerrno));

  pr_cmd_set_errno(cmd, xerrno);
  errno = xerrno;
  return PR_ERROR(cmd);
}

MODRET digest_opts_hash(cmd_rec *cmd) {
  char *algo_name;

  if (digest_engine == FALSE) {
    return PR_DECLINED(cmd);
  }

  if (cmd->argc > 2) {
    pr_response_add_err(R_501, _("OPTS HASH: Wrong number of parameters"));
    return PR_ERROR(cmd);
  }

  if (cmd->argc == 1) {
    int flags = DIGEST_ALGO_FL_IANA_STYLE;

    /* Client is querying the current hash algorithm */
    pr_response_add(R_200, "%s", get_algo_name(digest_hash_algo, flags));
    return PR_HANDLED(cmd);
  }

  /* Client is setting/changing the current hash algorithm. */

  algo_name = cmd->argv[1];

  if (strcasecmp(algo_name, "CRC32") == 0) {
    if (digest_algos & DIGEST_ALGO_CRC32) {
      digest_hash_algo = DIGEST_ALGO_CRC32;
      digest_hash_md = get_algo_md(digest_hash_algo);

    } else {
      pr_response_add_err(R_501, _("%s: Unsupported algorithm"), algo_name);
      return PR_ERROR(cmd);
    }

#ifndef OPENSSL_NO_MD5
  } else if (strcasecmp(algo_name, "MD5") == 0) {
    if (digest_algos & DIGEST_ALGO_MD5) {
      digest_hash_algo = DIGEST_ALGO_MD5;
      digest_hash_md = get_algo_md(digest_hash_algo);

    } else {
      pr_response_add_err(R_501, _("%s: Unsupported algorithm"), algo_name);
      return PR_ERROR(cmd);
    }
#endif /* OPENSSL_NO_MD5 */

#ifndef OPENSSL_NO_SHA1
  } else if (strcasecmp(algo_name, "SHA-1") == 0) {
    if (digest_algos & DIGEST_ALGO_SHA1) {
      digest_hash_algo = DIGEST_ALGO_SHA1;
      digest_hash_md = get_algo_md(digest_hash_algo);

    } else {
      pr_response_add_err(R_501, _("%s: Unsupported algorithm"), algo_name);
      return PR_ERROR(cmd);
    }
#endif /* OPENSSL_NO_SHA1 */

#ifndef OPENSSL_NO_SHA256
  } else if (strcasecmp(algo_name, "SHA-256") == 0) {
    if (digest_algos & DIGEST_ALGO_SHA256) {
      digest_hash_algo = DIGEST_ALGO_SHA256;
      digest_hash_md = get_algo_md(digest_hash_algo);

    } else {
      pr_response_add_err(R_501, _("%s: Unsupported algorithm"), algo_name);
      return PR_ERROR(cmd);
    }
#endif /* OPENSSL_NO_SHA256 */

#ifndef OPENSSL_NO_SHA512
  } else if (strcasecmp(algo_name, "SHA-512") == 0) {
    if (digest_algos & DIGEST_ALGO_SHA512) {
      digest_hash_algo = DIGEST_ALGO_SHA512;
      digest_hash_md = get_algo_md(digest_hash_algo);

    } else {
      pr_response_add_err(R_501, _("%s: Unsupported algorithm"), algo_name);
      return PR_ERROR(cmd);
    }
#endif /* OPENSSL_NO_SHA512 */

  } else {
    pr_response_add_err(R_501, _("%s: Unsupported algorithm"), algo_name);
    return PR_ERROR(cmd);
  }

  digest_hash_feat_remove();
  digest_hash_feat_add(cmd->tmp_pool);

  pr_response_add(R_200, "%s", algo_name);
  return PR_HANDLED(cmd);
}

MODRET digest_pre_retr(cmd_rec *cmd) {
  config_rec *c;
  unsigned char use_sendfile = TRUE;

  if (digest_engine == FALSE) {
    return PR_DECLINED(cmd);
  }

  if (digest_caching == FALSE) {
    return PR_DECLINED(cmd);
  }

  if (session.sf_flags & SF_ASCII) {
    pr_trace_msg(trace_channel, 19,
      "%s: ASCII mode transfer (TYPE A) in effect, not computing/caching "
      "opportunistic digest for download", (char *) cmd->argv[0]);
    return PR_DECLINED(cmd);
  }

  if (digest_opts & DIGEST_OPT_NO_TRANSFER_CACHE) {
    pr_trace_msg(trace_channel, 19,
      "%s: NoTransferCache DigestOption in effect, not computing/caching "
      "opportunistic digest for download", (char *) cmd->argv[0]);
    return PR_DECLINED(cmd);
  }

  if (session.restart_pos > 0) {
    pr_trace_msg(trace_channel, 12,
      "REST %" PR_LU " sent before %s, declining to compute transfer digest",
      (pr_off_t) session.restart_pos, (char *) cmd->argv[0]);
    return PR_DECLINED(cmd);
  }

  /* If UseSendfile is in effect, then we cannot watch the outbound traffic.
   * Note that UseSendfile is enabled by default.
   */
  c = find_config(CURRENT_CONF, CONF_PARAM, "UseSendfile", FALSE);
  if (c != NULL) {
    use_sendfile = *((unsigned char *) c->argv[0]);
  }

  if (use_sendfile) {
    pr_trace_msg(trace_channel, 12,
      "UseSendfile in effect, declining to compute digest for %s transfer",
      (char *) cmd->argv[0]);
    return PR_DECLINED(cmd);
  }

  digest_cache_xfer_ctx = EVP_MD_CTX_create();
  if (EVP_DigestInit_ex(digest_cache_xfer_ctx, digest_hash_md, NULL) != 1) {
    pr_trace_msg(trace_channel, 3,
      "error preparing %s digest: %s", get_algo_name(digest_hash_algo, 0),
      get_errors());
    EVP_MD_CTX_destroy(digest_cache_xfer_ctx);
    digest_cache_xfer_ctx = NULL;

  } else {
    pr_event_register(&digest_module, "core.data-write", digest_data_xfer_ev,
      digest_cache_xfer_ctx);
  }

  return PR_DECLINED(cmd);
}

MODRET digest_log_retr(cmd_rec *cmd) {
  const char *algo_name;
  unsigned char *digest;
  unsigned int digest_len;

  if (digest_engine == FALSE) {
    return PR_DECLINED(cmd);
  }

  if (digest_caching == FALSE ||
      (digest_opts & DIGEST_OPT_NO_TRANSFER_CACHE)) {
    return PR_DECLINED(cmd);
  }

  if (digest_cache_xfer_ctx == NULL) {
    /* Not sure how this would happen...? */
    return PR_DECLINED(cmd);
  }

  algo_name = get_algo_name(digest_hash_algo, 0);
  digest_len = EVP_MD_size(digest_hash_md);
  digest = palloc(cmd->tmp_pool, digest_len);

  if (EVP_DigestFinal_ex(digest_cache_xfer_ctx, digest, &digest_len) != 1) {
    pr_trace_msg(trace_channel, 1,
      "error finishing %s digest for %s: %s", algo_name,
      (char *) cmd->argv[0], get_errors());

  } else {
    int res;
    struct stat st;
    char *path;

    path = session.xfer.path;
    pr_fs_clear_cache2(path);
    res = pr_fsio_stat(path, &st);
    if (res == 0) {
      char *hex_digest;
      off_t start, len;
      time_t mtime;

      hex_digest = pr_str_bin2hex(cmd->tmp_pool, digest, digest_len,
        PR_STR_FL_HEX_USE_LC);

      mtime = st.st_mtime;
      start = 0;
      len = st.st_size;

      if (add_cached_digest(cmd->tmp_pool, digest_hash_algo, path, mtime,
          start, len, hex_digest) < 0) {
        pr_trace_msg(trace_channel, 8,
          "error caching %s digest for path '%s': %s", algo_name, path,
          strerror(errno));
      }

    } else {
      pr_trace_msg(trace_channel, 7,
        "error checking '%s' post-%s: %s", path, (char *) cmd->argv[0],
        strerror(errno));
    }
  }

  pr_event_unregister(&digest_module, "core.data-write", NULL);
  EVP_MD_CTX_destroy(digest_cache_xfer_ctx);
  digest_cache_xfer_ctx = NULL;

  return PR_DECLINED(cmd);
}

MODRET digest_log_retr_err(cmd_rec *cmd) {
  if (digest_engine == FALSE) {
    return PR_DECLINED(cmd);
  }

  if (digest_caching == FALSE ||
      (digest_opts & DIGEST_OPT_NO_TRANSFER_CACHE)) {
    return PR_DECLINED(cmd);
  }

  pr_event_unregister(&digest_module, "core.data-write", NULL);
  if (digest_cache_xfer_ctx != NULL) {
    EVP_MD_CTX_destroy(digest_cache_xfer_ctx);
    digest_cache_xfer_ctx = NULL;
  }

  return PR_DECLINED(cmd);
}

MODRET digest_pre_appe(cmd_rec *cmd) {
  int res;
  struct stat st;
  char *path;

  if (digest_engine == FALSE) {
    return PR_DECLINED(cmd);
  }

  if (digest_caching == FALSE) {
    return PR_DECLINED(cmd);
  }

  /* If we are appending to an existing file, then do NOT compute the digest;
   * we only do the opportunistic digest computation for complete files.  If
   * file exists, but is zero length, then do proceed with the computation.
   */

  path = pr_fs_decode_path(cmd->tmp_pool, cmd->arg);
  if (path == NULL) {
    return PR_DECLINED(cmd);
  }

  pr_fs_clear_cache2(path);
  res = pr_fsio_stat(path, &st);
  if (res == 0) {
    if (!S_ISREG(st.st_mode)) {
      /* Not a regular file. */
      return PR_DECLINED(cmd);
    }

    if (st.st_size > 0) {
      /* Not a zero length file. */
      return PR_DECLINED(cmd);
    }
  }

  if (session.sf_flags & SF_ASCII) {
    pr_trace_msg(trace_channel, 19,
      "%s: ASCII mode transfer (TYPE A) in effect, not computing/caching "
      "opportunistic digest for upload", (char *) cmd->argv[0]);
    return PR_DECLINED(cmd);
  }

  if (digest_opts & DIGEST_OPT_NO_TRANSFER_CACHE) {
    pr_trace_msg(trace_channel, 19,
      "%s: NoTransferCache DigestOption in effect, not computing/caching "
      "opportunistic digest for upload", (char *) cmd->argv[0]);
    return PR_DECLINED(cmd);
  }

  /* Does REST + APPE even make any sense? */
  if (session.restart_pos > 0) {
    pr_trace_msg(trace_channel, 12,
      "REST %" PR_LU " sent before %s, declining to compute transfer digest",
      (pr_off_t) session.restart_pos, (char *) cmd->argv[0]);
    return PR_DECLINED(cmd);
  }

  digest_cache_xfer_ctx = EVP_MD_CTX_create();
  if (EVP_DigestInit_ex(digest_cache_xfer_ctx, digest_hash_md, NULL) != 1) {
    pr_trace_msg(trace_channel, 3,
      "error preparing %s digest: %s", get_algo_name(digest_hash_algo, 0),
      get_errors());
    EVP_MD_CTX_destroy(digest_cache_xfer_ctx);
    digest_cache_xfer_ctx = NULL;

  } else {
    pr_event_register(&digest_module, "core.data-read", digest_data_xfer_ev,
      digest_cache_xfer_ctx);
  }

  return PR_DECLINED(cmd);
}

MODRET digest_pre_stor(cmd_rec *cmd) {
  if (digest_engine == FALSE) {
    return PR_DECLINED(cmd);
  }

  if (digest_caching == FALSE) {
    return PR_DECLINED(cmd);
  }

  if (session.sf_flags & SF_ASCII) {
    pr_trace_msg(trace_channel, 19,
      "%s: ASCII mode transfer (TYPE A) in effect, not computing/caching "
      "opportunistic digest for upload", (char *) cmd->argv[0]);
    return PR_DECLINED(cmd);
  }

  if (digest_opts & DIGEST_OPT_NO_TRANSFER_CACHE) {
    pr_trace_msg(trace_channel, 19,
      "%s: NoTransferCache DigestOption in effect, not computing/caching "
      "opportunistic digest for upload", (char *) cmd->argv[0]);
    return PR_DECLINED(cmd);
  }

  if (session.restart_pos > 0) {
    pr_trace_msg(trace_channel, 12,
      "REST %" PR_LU " sent before %s, declining to compute transfer digest",
      (pr_off_t) session.restart_pos, (char *) cmd->argv[0]);
    return PR_DECLINED(cmd);
  }

  digest_cache_xfer_ctx = EVP_MD_CTX_create();
  if (EVP_DigestInit_ex(digest_cache_xfer_ctx, digest_hash_md, NULL) != 1) {
    pr_trace_msg(trace_channel, 3,
      "error preparing %s digest: %s", get_algo_name(digest_hash_algo, 0),
      get_errors());
    EVP_MD_CTX_destroy(digest_cache_xfer_ctx);
    digest_cache_xfer_ctx = NULL;

  } else {
    pr_event_register(&digest_module, "core.data-read", digest_data_xfer_ev,
      digest_cache_xfer_ctx);
  }

  return PR_DECLINED(cmd);
}

MODRET digest_log_stor(cmd_rec *cmd) {
  const char *algo_name;
  unsigned char *digest;
  unsigned int digest_len;

  if (digest_engine == FALSE) {
    return PR_DECLINED(cmd);
  }

  if (digest_caching == FALSE ||
      (digest_opts & DIGEST_OPT_NO_TRANSFER_CACHE)) {
    return PR_DECLINED(cmd);
  }

  if (digest_cache_xfer_ctx == NULL) {
    /* Not sure how this would happen...? */
    return PR_DECLINED(cmd);
  }

  algo_name = get_algo_name(digest_hash_algo, 0);
  digest_len = EVP_MD_size(digest_hash_md);
  digest = palloc(cmd->tmp_pool, digest_len);

  if (EVP_DigestFinal_ex(digest_cache_xfer_ctx, digest, &digest_len) != 1) {
    pr_trace_msg(trace_channel, 1,
      "error finishing %s digest for %s: %s", algo_name, (char *) cmd->argv[0],
      get_errors());

  } else {
    int res;
    struct stat st;
    char *path;

    path = session.xfer.path;
    pr_fs_clear_cache2(path);
    res = pr_fsio_stat(path, &st);
    if (res == 0) {
      char *hex_digest;
      off_t start, len;
      time_t mtime;

      hex_digest = pr_str_bin2hex(cmd->tmp_pool, digest, digest_len,
        PR_STR_FL_HEX_USE_LC);

      mtime = st.st_mtime;
      start = 0;
      len = session.xfer.total_bytes;

      if (add_cached_digest(cmd->tmp_pool, digest_hash_algo, path, mtime,
          start, len, hex_digest) < 0) {
        pr_trace_msg(trace_channel, 8,
          "error caching %s digest for path '%s': %s", algo_name, path,
          strerror(errno));
      }

    } else {
      pr_trace_msg(trace_channel, 7,
        "error checking '%s' post-%s: %s", path, (char *) cmd->argv[0],
        strerror(errno));
    }
  }

  pr_event_unregister(&digest_module, "core.data-read", NULL);
  EVP_MD_CTX_destroy(digest_cache_xfer_ctx);
  digest_cache_xfer_ctx = NULL;

  return PR_DECLINED(cmd);
}

MODRET digest_log_stor_err(cmd_rec *cmd) {
  if (digest_engine == FALSE) {
    return PR_DECLINED(cmd);
  }

  if (digest_caching == FALSE ||
      (digest_opts & DIGEST_OPT_NO_TRANSFER_CACHE)) {
    return PR_DECLINED(cmd);
  }

  pr_event_unregister(&digest_module, "core.data-read", NULL);
  if (digest_cache_xfer_ctx != NULL) {
    EVP_MD_CTX_destroy(digest_cache_xfer_ctx);
    digest_cache_xfer_ctx = NULL;
  }

  return PR_DECLINED(cmd);
}

MODRET digest_post_pass(cmd_rec *cmd) {
  config_rec *c;

  if (digest_engine == FALSE) {
    return PR_DECLINED(cmd);
  }

  c = find_config(CURRENT_CONF, CONF_PARAM, "DigestEngine", FALSE);
  if (c != NULL) {
    digest_engine = *((int *) c->argv[0]);
  }

  if (digest_engine == FALSE) {
    return PR_DECLINED(cmd);
  }
  
  c = find_config(CURRENT_CONF, CONF_PARAM, "DigestAlgorithms", FALSE);
  if (c != NULL) {
    digest_algos = *((unsigned long *) c->argv[0]);
  }

  c = find_config(CURRENT_CONF, CONF_PARAM, "DigestCache", FALSE);
  if (c != NULL) {
    digest_caching = *((int *) c->argv[0]);
    digest_cache_max_size = *((size_t *) c->argv[1]);
  }

  return PR_DECLINED(cmd);
}

MODRET digest_xcrc(cmd_rec *cmd) {
  unsigned long algo = DIGEST_ALGO_CRC32;

  if (digest_isenabled(algo) != TRUE) {
    pr_log_debug(DEBUG9, MOD_DIGEST_VERSION
      ": unable to handle %s command: CRC32 disabled by DigestAlgorithms",
      (char *) cmd->argv[0]);
    return PR_DECLINED(cmd);
  }

  return digest_xcmd(cmd, algo);
}

MODRET digest_xmd5(cmd_rec *cmd) {
  unsigned long algo = DIGEST_ALGO_MD5;

  if (digest_isenabled(algo) != TRUE) {
    pr_log_debug(DEBUG9, MOD_DIGEST_VERSION
      ": unable to handle %s command: MD5 disabled by DigestAlgorithms",
      (char *) cmd->argv[0]);
    return PR_DECLINED(cmd);
  }

  return digest_xcmd(cmd, algo);
}

MODRET digest_xsha1(cmd_rec *cmd) {
  unsigned long algo = DIGEST_ALGO_SHA1;

  if (digest_isenabled(algo) != TRUE) {
    pr_log_debug(DEBUG9, MOD_DIGEST_VERSION
      ": unable to handle %s command: SHA1 disabled by DigestAlgorithms",
      (char *) cmd->argv[0]);
    return PR_DECLINED(cmd);
  }

  return digest_xcmd(cmd, algo);
}

MODRET digest_xsha256(cmd_rec *cmd) {
  unsigned long algo = DIGEST_ALGO_SHA256;

  if (digest_isenabled(algo) != TRUE) {
    pr_log_debug(DEBUG9, MOD_DIGEST_VERSION
      ": unable to handle %s command: SHA256 disabled by DigestAlgorithms",
      (char *) cmd->argv[0]);
    return PR_DECLINED(cmd);
  }

  return digest_xcmd(cmd, algo);
}

MODRET digest_xsha512(cmd_rec *cmd) {
  unsigned long algo = DIGEST_ALGO_SHA512;

  if (digest_isenabled(algo) != TRUE) {
    pr_log_debug(DEBUG9, MOD_DIGEST_VERSION
      ": unable to handle %s command: SHA512 disabled by DigestAlgorithms",
      (char *) cmd->argv[0]);
    return PR_DECLINED(cmd);
  }

  return digest_xcmd(cmd, algo);
}

/* Event listeners
 */

static void digest_data_xfer_ev(const void *event_data, void *user_data) {
  const pr_buffer_t *pbuf;
  EVP_MD_CTX *md_ctx;

  pbuf = event_data;
  md_ctx = user_data;

  if (EVP_DigestUpdate(md_ctx, pbuf->buf, pbuf->buflen) != 1) {
    pr_trace_msg(trace_channel, 3,
      "error updating %s digest: %s", get_algo_name(digest_hash_algo, 0),
      get_errors());

  } else {
    pr_trace_msg(trace_channel, 19,
      "updated %s digest with %lu bytes", get_algo_name(digest_hash_algo, 0),
      (unsigned long) pbuf->buflen);
  }
}

#if defined(PR_SHARED_MODULE)
static void digest_mod_unload_ev(const void *event_data, void *user_data) {
  if (strcmp((char *) event_data, "mod_digest.c") == 0) {
    pr_event_unregister(&digest_module, NULL);
  }
}
#endif /* PR_SHARED_MODULE */

/* Initialization routines
 */

static int digest_init(void) {
  digest_pool = make_sub_pool(permanent_pool);
  pr_pool_tag(digest_pool, MOD_DIGEST_VERSION);

#if defined(PR_SHARED_MODULE)
  pr_event_register(&digest_module, "core.module-unload", digest_mod_unload_ev,
    NULL);
#endif /* PR_SHARED_MODULE */

  return 0;
}

static int digest_sess_init(void) {
  config_rec *c;

  c = find_config(main_server->conf, CONF_PARAM, "DigestEngine", FALSE);
  if (c != NULL) {
    digest_engine = *((int *) c->argv[0]);
  }

  if (digest_engine == FALSE) {
    return 0;
  }

  c = find_config(main_server->conf, CONF_PARAM, "DigestAlgorithms", FALSE);
  if (c != NULL) {
    digest_algos = *((unsigned long *) c->argv[0]);
  }

  /* Use the configured algorithms to determine our default HASH; it may be
   * that SHA1 is disabled or not available via OpenSSL.
   *
   * Per the HASH Draft, the default HASH SHOULD be SHA1; if not, it should
   * be a "stronger" HASH function.  Thus this ordering may not be what you
   * would expect.
   */

  if (digest_algos & DIGEST_ALGO_SHA1) {
    digest_hash_algo = DIGEST_ALGO_SHA1;

  } else if (digest_algos & DIGEST_ALGO_SHA256) {
    digest_hash_algo = DIGEST_ALGO_SHA256;

  } else if (digest_algos & DIGEST_ALGO_SHA512) {
    digest_hash_algo = DIGEST_ALGO_SHA512;

  } else if (digest_algos & DIGEST_ALGO_MD5) {
    digest_hash_algo = DIGEST_ALGO_MD5;

  } else {
    /* We are GUARANTEED to always be able to do CRC32. */
    digest_hash_algo = DIGEST_ALGO_CRC32;
  }

  digest_hash_md = get_algo_md(digest_hash_algo);

  c = find_config(main_server->conf, CONF_PARAM, "DigestCache", FALSE);
  if (c != NULL) {
    digest_caching = *((int *) c->argv[0]);
    digest_cache_max_size = *((size_t *) c->argv[1]);
  }

  c = find_config(main_server->conf, CONF_PARAM, "DigestOptions", FALSE);
  while (c != NULL) {
    unsigned long opts = 0;

    pr_signals_handle();

    opts = *((unsigned long *) c->argv[0]);
    digest_opts |= opts;

    c = find_config_next(c, c->next, CONF_PARAM, "DigestOptions", FALSE);
  }

  if (digest_caching == TRUE) {
    digest_crc32_tab = pr_table_alloc(digest_pool, 0);
    digest_md5_tab = pr_table_alloc(digest_pool, 0);
    digest_sha1_tab = pr_table_alloc(digest_pool, 0);
    digest_sha256_tab = pr_table_alloc(digest_pool, 0);
    digest_sha512_tab = pr_table_alloc(digest_pool, 0);
  }

  digest_hash_feat_add(session.pool);
  pr_help_add(C_HASH, _("<sp> pathname"), TRUE);

  digest_x_feat_add(session.pool);
  digest_x_help_add(session.pool);

  return 0;
}

/* Module API tables
 */

static cmdtable digest_cmdtab[] = {
  { CMD, C_HASH,	G_READ, digest_hash,	TRUE, FALSE, CL_READ|CL_INFO },
  { CMD, C_OPTS"_HASH",	G_NONE,	digest_opts_hash,FALSE,FALSE },

  { CMD, C_XCRC,	G_READ, digest_xcrc,	TRUE, FALSE, CL_READ|CL_INFO },
  { CMD, C_XMD5,	G_READ, digest_xmd5,	TRUE, FALSE, CL_READ|CL_INFO },
  { CMD, C_XSHA,	G_READ, digest_xsha1,	TRUE, FALSE, CL_READ|CL_INFO },
  { CMD, C_XSHA1,	G_READ, digest_xsha1,	TRUE, FALSE, CL_READ|CL_INFO },
  { CMD, C_XSHA256,	G_READ, digest_xsha256,	TRUE, FALSE, CL_READ|CL_INFO },
  { CMD, C_XSHA512,	G_READ, digest_xsha512,	TRUE, FALSE, CL_READ|CL_INFO },

  { POST_CMD,	C_PASS, G_NONE,	digest_post_pass, TRUE, FALSE },

  /* Command handlers for opportunistic digest computation/caching. */
  { PRE_CMD,	C_APPE, G_NONE, digest_pre_appe,	TRUE, FALSE },
  { LOG_CMD,	C_APPE, G_NONE, digest_log_stor,	TRUE, FALSE },
  { LOG_CMD_ERR,C_APPE, G_NONE, digest_log_stor_err,	TRUE, FALSE },
  { PRE_CMD,	C_RETR, G_NONE, digest_pre_retr,	TRUE, FALSE },
  { LOG_CMD,	C_RETR, G_NONE, digest_log_retr,	TRUE, FALSE },
  { LOG_CMD_ERR,C_RETR, G_NONE, digest_log_retr_err,	TRUE, FALSE },
  { PRE_CMD,	C_STOR, G_NONE, digest_pre_stor,	TRUE, FALSE },
  { LOG_CMD,	C_STOR, G_NONE, digest_log_stor,	TRUE, FALSE },
  { LOG_CMD_ERR,C_STOR, G_NONE, digest_log_stor_err,	TRUE, FALSE },

  { 0, NULL }
};

static conftable digest_conftab[] = {
  { "DigestAlgorithms",	set_digestalgorithms,	NULL },
  { "DigestCache",	set_digestcache,	NULL },
  { "DigestEngine",	set_digestengine,	NULL },
  { "DigestMaxSize",	set_digestmaxsize,	NULL },
  { "DigestOptions",	set_digestoptions,	NULL },

  { NULL }
};

module digest_module = {
  NULL, NULL,

  /* Module API version */
  0x20,

  /* Module name */
  "digest",

  /* Module configuration table */
  digest_conftab,

  /* Module command handler table */
  digest_cmdtab,

  /* Module auth handler table */
  NULL,

  /* Module initialization function */
  digest_init,

  /* Session initialization function */
  digest_sess_init,

  /* Module version */
  MOD_DIGEST_VERSION
};
