/* Copyright (c) 2022 Simo Sorce <simo@redhat.com> - see COPYING */

#include "provider.h"
#include <string.h>

struct p11prov_uri {
    char *model;
    char *manufacturer;
    char *token;
    char *serial;
    char *object;
    unsigned char *id;
    size_t id_len;
    char *pin;
    CK_OBJECT_CLASS class;
};

struct p11prov_object {
    PROVIDER_CTX *provctx;
    struct p11prov_uri *parsed_uri;
    int loaded;

    struct p11prov_key *priv_key;
    struct p11prov_key *pub_key;

    CK_SESSION_HANDLE login_session;

    int refcnt;
};

static void p11prov_uri_free(struct p11prov_uri *parsed_uri)
{
    if (parsed_uri == NULL) return;

    OPENSSL_free(parsed_uri->model);
    OPENSSL_free(parsed_uri->manufacturer);
    OPENSSL_free(parsed_uri->token);
    OPENSSL_free(parsed_uri->serial);
    OPENSSL_free(parsed_uri->object);
    OPENSSL_free(parsed_uri->id);
    if (parsed_uri->pin) {
        OPENSSL_clear_free(parsed_uri->pin, strlen(parsed_uri->pin));
    }
    OPENSSL_clear_free(parsed_uri, sizeof(struct p11prov_uri));
}

static P11PROV_OBJECT *p11prov_object_ref(P11PROV_OBJECT *obj)
{
    if (obj &&
        __atomic_fetch_add(&obj->refcnt, 1, __ATOMIC_ACQ_REL) > 0) {
        return obj;
    }

    return NULL;
}

void p11prov_object_free(P11PROV_OBJECT *obj)
{
    p11prov_debug("object free (%p)\n", obj);

    if (obj == NULL) return;
    if (__atomic_sub_fetch(&obj->refcnt, 1, __ATOMIC_ACQ_REL) != 0) {
        p11prov_debug("object free: reference held\n");
        return;
    }

    if (obj->login_session != CK_INVALID_HANDLE) {
        CK_FUNCTION_LIST_PTR f = provider_ctx_fns(obj->provctx);
        if (f) {
            (void)f->C_CloseSession(obj->login_session);
        }
    }

    p11prov_uri_free(obj->parsed_uri);
    p11prov_key_free(obj->priv_key);
    p11prov_key_free(obj->pub_key);

    OPENSSL_clear_free(obj, sizeof(P11PROV_OBJECT));
}

bool p11prov_object_check_key(P11PROV_OBJECT *obj, bool priv)
{
    if (priv) {
        return obj->priv_key != NULL;
    }
    return obj->pub_key != NULL;
}

P11PROV_KEY *p11prov_object_get_key(P11PROV_OBJECT *obj, bool priv)
{
    if (priv) {
        return p11prov_key_ref(obj->priv_key);
    }
    return p11prov_key_ref(obj->pub_key);
}

int p11prov_object_export_public_rsa_key(P11PROV_OBJECT *obj,
                                         OSSL_CALLBACK *cb_fn, void *cb_arg)
{
    OSSL_PARAM params[3];
    CK_ATTRIBUTE *n, *e;
    int pidx = 0;
    int ret = 0;

    if (p11prov_key_type(obj->pub_key) != CKK_RSA) return RET_OSSL_ERR;

    n = p11prov_key_attr(obj->pub_key, CKA_MODULUS);
    if (n == NULL) return RET_OSSL_ERR;
    params[0] = OSSL_PARAM_construct_BN(OSSL_PKEY_PARAM_RSA_N,
                                        n->pValue,
                                        n->ulValueLen);

    e = p11prov_key_attr(obj->pub_key, CKA_PUBLIC_EXPONENT);
    if (e == NULL) return RET_OSSL_ERR;
    params[1] = OSSL_PARAM_construct_BN(OSSL_PKEY_PARAM_RSA_E,
                                        e->pValue,
                                        e->ulValueLen);

    params[2] = OSSL_PARAM_construct_end();

    return cb_fn(params, cb_arg);
}

static int hex_to_byte(const char *in, unsigned char *byte)
{
    char c[2], s;
    int i = 0;

    for (i = 0; i < 2; i++) {
        s = in[i];
        if ('0' <= s && s <= '9') {
            c[i] = s - '0';
        } else if ('a' <= s && s <= 'f') {
            c[i] = s - 'a' + 10;
        } else if ('A' <= s && s <= 'F') {
            c[i] = s - 'A' + 10;
        } else {
            return EINVAL;
        }
    }
    *byte = (c[0] << 4) | c[1];
    return 0;
}

static int parse_attr(const char *str, size_t len,
                      unsigned char **output, size_t *outlen)
{
    unsigned char *out;
    size_t index = 0;
    int ret;

    out = OPENSSL_malloc(len + 1);
    if (out == NULL) {
        return ENOMEM;
    }

    while (*str && len > 0) {
        if (*str == '%') {
            char hex[3] = { 0 };
            if (len < 3) {
                ret = EINVAL;
                goto done;
            }
            hex[0] = str[1];
            hex[1] = str[2];
            ret = hex_to_byte(hex, &out[index]);
            if (ret != 0) goto done;

            index++;
            str += 3;
            len -= 3;
        } else {
            out[index] = *str;
            index++;
            str++;
            len--;
        }
    }

    out[index] = '\0';
    ret = 0;

done:
    if (ret != 0) {
        OPENSSL_free(out);
    } else {
        *output = out;
        *outlen = index;
    }
    return ret;
}

#define MAX_PIN_LENGTH 32
static int get_pin(const char *str, size_t len,
                   char **output, size_t *outlen)
{
    char pin[MAX_PIN_LENGTH+1];
    char *pinfile;
    char *filename;
    BIO *fp;
    int ret;

    ret = parse_attr(str, len, (unsigned char **)&pinfile, outlen);
    if (ret != 0) return ret;

    if (strncmp((const char *)pinfile, "file:", 5) == 0) {
        filename = pinfile + 5;
    } else if (*pinfile == '|') {
        ret = EINVAL;
        goto done;
    } else {
        /* missing 'file:' is accepted */
        filename = pinfile;
    }

    fp = BIO_new_file(filename, "r");
    if (fp == NULL) {
        p11prov_debug("Failed to get pin from %s\n", filename);
        ret = ENOENT;
        goto done;
    }
    ret = BIO_gets(fp, pin, MAX_PIN_LENGTH);
    if (ret <= 0) {
        p11prov_debug("Failed to get pin from %s (%d)\n", filename, ret);
        ret = EINVAL;
        BIO_free(fp);
        goto done;
    }
    BIO_free(fp);

    *output = OPENSSL_strdup(pin);
    if (*output == NULL) {
        ret = ENOMEM;
        goto done;
    }

    ret = 0;
done:
    OPENSSL_free(pinfile);
    return ret;
}

static int parse_uri(struct p11prov_uri *u, const char *uri)
{
    const char *p, *end;
    int ret;

    if (strncmp(uri, "pkcs11:", 7) != 0) {
        return EINVAL;
    }

    p = uri + 7;
    while (p) {
        size_t outlen;
        unsigned char **ptr;
        size_t *ptrlen;
        size_t len;

        end = strpbrk(p, ";?&");
        if (end) {
            len = end - p;
        } else {
            len = strlen(p);
        }

        ptr = NULL;
        ptrlen = &outlen;

        if (strncmp(p, "model=", 6) == 0) {
            p += 6;
            len -= 6;
            ptr = (unsigned char **)&u->model;
        } else if (strncmp(p, "manufacturer=", 13) == 0) {
            p += 13;
            len -= 13;
            ptr = (unsigned char **)&u->manufacturer;
        } else if (strncmp(p, "token=", 6) == 0) {
            p += 6;
            len -= 6;
            ptr = (unsigned char **)&u->token;
        } else if (strncmp(p, "serial=", 7) == 0) {
            p += 7;
            len -= 7;
            ptr = (unsigned char **)&u->object;
        } else if (strncmp(p, "id=", 3) == 0) {
            p += 3;
            len -= 3;
            ptr = &u->id;
            ptrlen = &u->id_len;
        } else if (strncmp(p, "pin-value=", 10) == 0) {
            p += 10;
            len -= 10;
            ptr = (unsigned char **)&u->pin;
        } else if (strncmp(p, "pin-source=", 11) == 0) {
            p += 11;
            len -= 11;
            ret = get_pin(p, len, &u->pin, ptrlen);
            if (ret != 0) goto done;
        } else if (strncmp(p, "type=", 5) == 0 ||
                   strncmp(p, "object-type=", 12) == 0) {
            p += 4;
            if (*p == '=') {
                p++;
                len -= 5;
            } else {
                p += 8;
                len -= 12;
            }
            if (len == 4 && strncmp(p, "cert", 4) == 0) {
                u->class = CKO_CERTIFICATE;
            } else if (len == 6 && strncmp(p, "public", 6) == 0) {
                u->class = CKO_PUBLIC_KEY;
            } else if (len == 7 && strncmp(p, "private", 7) == 0) {
                u->class = CKO_PRIVATE_KEY;
            } else {
                p11prov_debug("Unknown object type\n");
                ret = EINVAL;
                goto done;
            }
        } else {
            p11prov_debug("Ignoring unkown pkcs11 URI attribute\n");
        }

        if (ptr) {
            ret = parse_attr(p, len, ptr, ptrlen);
            if (ret != 0) goto done;
        }

        if (end) {
            p = end + 1;
        } else {
            p = NULL;
        }
    }

    ret = 0;
done:
    return ret;
}

DISPATCH_STORE_FN(open);
DISPATCH_STORE_FN(attach);
DISPATCH_STORE_FN(load);
DISPATCH_STORE_FN(eof);
DISPATCH_STORE_FN(close);
DISPATCH_STORE_FN(export_object);

static void *p11prov_store_open(void *provctx, const char *uri)
{
    PROVIDER_CTX *ctx = (PROVIDER_CTX *)provctx;
    P11PROV_OBJECT *obj;
    int ret;

    p11prov_debug("object open (%p, %s)\n", ctx, uri);

    obj = OPENSSL_zalloc(sizeof(P11PROV_OBJECT));
    if (obj == NULL) return NULL;

    obj->parsed_uri = OPENSSL_zalloc(sizeof(struct p11prov_uri));
    if (obj->parsed_uri == NULL) {
        p11prov_object_free(obj);
        return NULL;
    }

    ret = parse_uri(obj->parsed_uri, uri);
    if (ret != 0) {
        p11prov_object_free(obj);
        return NULL;
    }

    obj->provctx = ctx;
    obj->refcnt = 1;

    return obj;
}

static void *p11prov_store_attach(void *provctx, OSSL_CORE_BIO *in)
{
    PROVIDER_CTX *ctx = (PROVIDER_CTX *)provctx;

    p11prov_debug("object attach (%p, %p)\n", ctx, in);

    return NULL;
}

static int p11prov_store_load(void *ctx,
                              OSSL_CALLBACK *object_cb, void *object_cbarg,
                              OSSL_PASSPHRASE_CALLBACK *pw_cb, void *pw_cbarg)
{
    P11PROV_OBJECT *obj = (P11PROV_OBJECT *)ctx;
    struct p11prov_slot *slots = NULL;
    int nslots = 0;

    p11prov_debug("object load (%p)\n", obj);

    nslots = provider_ctx_lock_slots(obj->provctx, &slots);

    for (int i = 0; i < nslots; i++) {
	CK_TOKEN_INFO token;

        /* ignore slots that are not initialized */
        if (slots[i].slot.flags & CKF_TOKEN_PRESENT == 0) continue;
        if (slots[i].token.flags & CKF_TOKEN_INITIALIZED == 0) continue;

        token = slots[i].token;

        /* skip slots that do not match */
        if (obj->parsed_uri->model &&
            strncmp(obj->parsed_uri->model, token.model, 16) != 0)
            continue;
        if (obj->parsed_uri->manufacturer &&
            strncmp(obj->parsed_uri->manufacturer,
                    token.manufacturerID, 32) != 0)
            continue;
        if (obj->parsed_uri->token &&
            strncmp(obj->parsed_uri->token, token.label, 32) != 0)
            continue;
        if (obj->parsed_uri->serial &&
            strncmp(obj->parsed_uri->serial, token.serialNumber, 16) != 0)
            continue;

        if (token.flags & CKF_LOGIN_REQUIRED) {
            CK_FUNCTION_LIST *f = provider_ctx_fns(obj->provctx);
            CK_UTF8CHAR_PTR pin = obj->parsed_uri->pin;
            CK_ULONG pinlen = 0;
            int ret;

            if (f == NULL) return RET_OSSL_ERR;
            if (pin) pinlen = strlen(pin);

            if (obj->login_session == CK_INVALID_HANDLE) {
                ret = f->C_OpenSession(slots[i].id, CKF_SERIAL_SESSION, NULL,
                                       NULL, &obj->login_session);
                if (ret != CKR_OK) {
                    p11prov_debug("OpenSession failed %d\n", ret);
                    /* TODO: Err message */
                    continue;
                }
            }

            /* Supports only USER login sessions for now */
            ret = f->C_Login(obj->login_session, CKU_USER, pin, pinlen);
            if (ret && ret != CKR_USER_ALREADY_LOGGED_IN) {
                p11prov_debug("C_Login failed (%d)\n", ret);
                continue;
            }
        }

        /* match class */
        if (obj->parsed_uri->class == CKO_CERTIFICATE) {
            /* not yet */
            continue;
        } else if (obj->parsed_uri->class == CKO_PUBLIC_KEY ||
                   obj->parsed_uri->class == CKO_PRIVATE_KEY) {
             int ret = find_keys(obj->provctx,
                                 &obj->priv_key,
                                 &obj->pub_key,
                                 slots[i].id,
                                 obj->parsed_uri->class,
                                 obj->parsed_uri->id,
                                 obj->parsed_uri->id_len,
                                 obj->parsed_uri->object);
            /* for keys return on first match */
            if (ret == CKR_OK) break;
        }
    }

    provider_ctx_unlock_slots(obj->provctx, &slots);

    obj->loaded = 1;

    if (obj->pub_key || obj->priv_key) {
        OSSL_PARAM params[4];
        int object_type = OSSL_OBJECT_PKEY;
        CK_KEY_TYPE type;
        char *data_type;

        params[0] = OSSL_PARAM_construct_int(
                        OSSL_OBJECT_PARAM_TYPE, &object_type);

        /* we only support RSA so far */
        if (obj->pub_key) {
            type = p11prov_key_type(obj->pub_key);
        } else {
            type = p11prov_key_type(obj->priv_key);
        }
        switch (type) {
        case CKK_RSA:
            /* we have to handle private keys as our own type,
             * while we can let openssl import public keys and
             * deal with them in the default provider */
            if (obj->priv_key) data_type = P11PROV_NAMES_RSA;
            else data_type = "RSA";
            break;
        default:
            return RET_OSSL_ERR;
        }
        params[1] = OSSL_PARAM_construct_utf8_string(
                        OSSL_OBJECT_PARAM_DATA_TYPE, data_type, 0);

        /* giving away the object by reference */
        params[2] = OSSL_PARAM_construct_octet_string(
                        OSSL_OBJECT_PARAM_REFERENCE,
                        p11prov_object_ref(obj), sizeof(obj));
        params[3] = OSSL_PARAM_construct_end();

        return object_cb(params, object_cbarg);
    }

    return RET_OSSL_ERR;
}

static int p11prov_store_eof(void *ctx)
{
    P11PROV_OBJECT *obj = (P11PROV_OBJECT *)ctx;

    p11prov_debug("object eof (%p)\n", obj);

    return obj->loaded?1:0;
}

static int p11prov_store_close(void *ctx)
{
    P11PROV_OBJECT *obj = (P11PROV_OBJECT *)ctx;

    p11prov_debug("object close (%p)\n", obj);

    if (obj == NULL) return 0;

    p11prov_object_free(obj);
    return 1;
}

static int p11prov_store_set_ctx_params(void *loaderctx,
                                        const OSSL_PARAM params[])
{
    p11prov_debug("set ctx params (%p, %p)\n", loaderctx, params);

    return 1;
}

static int p11prov_store_export_object(void *loaderctx,
                                       const void *reference,
                                       size_t reference_sz,
                                       OSSL_CALLBACK *cb_fn,
                                       void *cb_arg)
{
    P11PROV_OBJECT *obj = NULL;

    p11prov_debug("object export %p, %ld\n", reference, reference_sz);

    if (!reference || reference_sz != sizeof(obj))
        return 0;

    /* the contents of the reference is the address to our object */
    obj = (P11PROV_OBJECT *)reference;

    /* we can only export public bits, so that's all we do */
    return p11prov_object_export_public_rsa_key(obj, cb_fn, cb_arg);
}

const OSSL_DISPATCH p11prov_store_functions[] = {
    DISPATCH_STORE_ELEM(OPEN, open),
    DISPATCH_STORE_ELEM(ATTACH, attach),
    DISPATCH_STORE_ELEM(LOAD, load),
    DISPATCH_STORE_ELEM(EOF, eof),
    DISPATCH_STORE_ELEM(CLOSE, close),
    DISPATCH_STORE_ELEM(SET_CTX_PARAMS, set_ctx_params),
    DISPATCH_STORE_ELEM(EXPORT_OBJECT, export_object),
    { 0, NULL }
};

