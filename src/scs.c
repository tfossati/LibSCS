/*
 * (c) KoanLogic Srl - 2011
 */ 

#include <assert.h>
#include <stdio.h>
#include <stdlib.h> 
#include <time.h>
#include <string.h>
#include "scs.h"
#include "conf.h"
#ifdef HAVE_LIBZ
  #include <zlib.h>
#endif  /* HAVE_LIBZ */

#ifdef USE_CYASSL
  #include "cyassl_drv.h"
#elif defined(USE_OPENSSL)
  #include "openssl_drv.h"
#endif  /* USE_CYASSL | USE_OPENSSL */

static struct
{
    int (*init) (void);
    int (*gen_iv) (scs_t *scs);
    int (*enc) (scs_t *scs, uint8_t *in, size_t in_sz, uint8_t *out);
    int (*tag) (scs_t *scs);
    void (*term) (void);
} D = {
#ifdef USE_CYASSL
    cyassl_init,
    cyassl_gen_iv,
    cyassl_enc,
    cyassl_tag,
    cyassl_term
#elif defined (USE_OPENSSL)
    /* TODO */
#endif
};

static int init_keyset (scs_keyset_t *keyset, const char *tid, 
        scs_cipherset_t cipherset, const uint8_t *key, const uint8_t *hkey, 
        int comp);
static int comp (const char *in, uint8_t *out, size_t *pout_sz);
static int pad (size_t block_sz, uint8_t *b, size_t *sz, size_t capacity);
static void print_buf (const char *label, const uint8_t *b, size_t b_sz);

/**
 *  \brief  Return an SCS context initialized with the supplied tid, keyset,
 *          cipherset, compression and max age parameters.
 *          Note that the \p tid string must be at least 64 bytes long: longer
 *          strings will be silently truncated.
 *          In case no deflate library has been found during the configuration
 *          stage, the \p comp value will be silently ignored (always set to 
 *          false). 
 */ 
int scs_init (const char *tid, scs_cipherset_t cipherset, const uint8_t *key, 
        const uint8_t *hkey, int comp, time_t max_session_age, scs_t **ps)
{
    scs_t *s = NULL;
    scs_err_t rc;

    /* TODO preconditions. */

    if ((s = malloc(sizeof *s)) == NULL)
        return SCS_ERR_MEM;

    /* Initialize current keyset. */
    rc = init_keyset(&s->cur_keyset, tid, cipherset, key, hkey, comp);
    if (rc != SCS_OK)
        goto err;

    s->data = NULL; /* Initialize it expliticly, so that free(3) won't die on 
                       scs_reset_atoms(). */

    s->max_session_age = max_session_age;

    s->cur_keyset.in_use = 1;
    s->prev_keyset.in_use = 0;

    *ps = s;

    return SCS_OK;
err:
    free(s);
    return rc;
}

/** 
 *  \brief  Dispose memory resources allocated to the supplied SCS context.
 */
void scs_term (scs_t *s)
{
    if (s)
    {
        /* XXX secure key deletion of keying material ? */
        free(s);
    }

    return;
}

/**
 *  \brief  ...
 */ 
int scs_outbound (scs_t *scs, const char *state)
{
    scs_err_t rc;
    size_t state_sz;
    scs_keyset_t *ks;

    /* TODO preconditions. */

    /* Cleanup protocol atoms from a previous run. */
    scs_reset_atoms(scs);

    /* Working keyset is the current. */
    ks = &scs->cur_keyset;

    /* 1. iv = RAND() */
    if (D.gen_iv(scs))
        return SCS_ERR_CRYPTO;

    /*  2. atime = NOW */
    if ((scs->atime = time(NULL)) == (time_t) -1)
        return SCS_ERR_OS;

    /* Make room for the working buffer, taking care for extra padding space. */
    state_sz = strlen(state);
    scs->data_capacity = state_sz + 100;

    if ((scs->data = malloc(scs->data_capacity)) == NULL)
        return SCS_ERR_MEM;

    /* 3.1 Comp(state) [OPTIONAL] */
    if (!ks->comp)
    {
        scs->data_sz = state_sz;
        memcpy(scs->data, (uint8_t *) state, scs->data_sz);
    }
    else if ((rc = comp(state, scs->data, &scs->data_sz)) != SCS_OK)
        goto err;

    /* Pad data if needed. */
    rc = pad(ks->block_sz, scs->data, &scs->data_sz, scs->data_capacity);
    if (rc != SCS_OK)
        goto err;

    /* 3.2. data = Enc(Comp(state)) 
     * 4. tag = HMAC(data||atime||tid||iv) */
    if (D.enc(scs, scs->data, scs->data_sz, scs->data) || D.tag(scs))
    {
        rc = SCS_ERR_CRYPTO;
        goto err;
    }

    print_buf("TAG", scs->tag, sizeof scs->tag);

    return SCS_OK;
err:
    scs_reset_atoms(scs);   /* Cleanup garbage. */
    return rc;
}

/**
 *  \brief  Reset protocol atoms values.
 */ 
void scs_reset_atoms (scs_t *scs)
{
    if (scs == NULL)
        return;

    free(scs->data);
    scs->data = NULL;
    scs->data_sz = scs->data_capacity = 0;
    scs->tag_sz = 0;
    scs->atime = (time_t) -1;

    return;
}

int scs_inbound (scs_t *scs)
{
    /* TODO */
    return 0;
}

static int comp (const char *in, uint8_t *out, size_t *pout_sz)
{
#ifdef HAVE_LIBZ
    int ret;
    z_stream zstr;

    /* TODO preconditions. */

    /* Allocate deflate in (using the default memory manager.) */
    zstr.zalloc = Z_NULL;
    zstr.zfree = Z_NULL;
    zstr.opaque = Z_NULL;

    if (deflateInit(&zstr, Z_DEFAULT_COMPRESSION) != Z_OK)
        return SCS_ERR_COMPRESSION;

    zstr.avail_in = strlen(in);
    zstr.next_in = (Bytef *) in;
    zstr.next_out = out;
    zstr.avail_out = sizeof out;

    ret = deflate(&zstr, Z_FINISH);
    if (ret != Z_OK && ret != Z_STREAM_END)
        return SCS_ERR_COMPRESSION;

    if ((ret = deflateEnd(&zstr)) != Z_OK)
        return SCS_ERR_COMPRESSION;

    *pout_sz = zstr.total_out;
#else
    assert(!"I'm not supposed to get there...");
#endif  /* HAVE_LIBZ */
    return SCS_OK;
}

static int pad (size_t block_sz, uint8_t *b, size_t *sz, size_t capacity)
{
    size_t pad_len = block_sz - (*sz % block_sz);

    /* Nothing to be changed. */
    if (pad_len == 0)
        return SCS_OK;

    /* TODO realloc ? assert ? */
    if (*sz + pad_len > capacity)
        return SCS_ERR_MEM;

    /* Pad with zeroes and update wbuf size. */
    memset(b + *sz, 0, pad_len);
    *sz += pad_len;

    return SCS_OK;
}

static void print_buf (const char *label, const uint8_t *b, size_t b_sz)
{
    unsigned int i;

    printf("<%s>", label);
    for (i = 0; i < b_sz; ++i)
    {
        if (i % 8 == 0)
            printf("\n");

        printf("%02X ", b[i]);
    }
    printf("\n</%s>\n", label);
}

static int init_keyset (scs_keyset_t *keyset, const char *tid, 
        scs_cipherset_t cipherset, const uint8_t *key, const uint8_t *hkey, 
        int comp)
{
    scs_err_t rc;

    /* Silently truncate tid if longer than sizeof keyset->tid (64 bytes). */
    (void) snprintf(keyset->tid, sizeof keyset->tid, "%s", tid);

    /* Set the compression flag as requested.  In case zlib is not available 
     * on the platform, ignore user request and set to false. */
#ifdef HAVE_LIBZ
    keyset->comp = comp;
#else
    keyset->comp = 0;
#endif  /* HAVE_LIBZ */

    /* Setup keyset and cipherset. */
    switch ((keyset->cipherset = cipherset))
    {
        case AES_128_CBC_HMAC_SHA1:
            keyset->key_sz = keyset->block_sz = 16;
            keyset->hkey_sz = 20;
            break;
        default:
            rc = SCS_ERR_WRONG_CIPHERSET;
            goto err;
    }

    /* Internalise keying material. */
    memcpy(keyset->key, key, keyset->key_sz);
    memcpy(keyset->hkey, hkey, keyset->hkey_sz);

    return SCS_OK;
err:
    return rc;
}
