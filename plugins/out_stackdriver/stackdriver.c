/* -*- Mode: C; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*- */

/*  Fluent Bit
 *  ==========
 *  Copyright (C) 2019      The Fluent Bit Authors
 *  Copyright (C) 2015-2018 Treasure Data Inc.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#include <fluent-bit/flb_info.h>
#include <fluent-bit/flb_output.h>
#include <fluent-bit/flb_http_client.h>
#include <fluent-bit/flb_pack.h>
#include <fluent-bit/flb_utils.h>
#include <fluent-bit/flb_time.h>
#include <fluent-bit/flb_oauth2.h>

#include <msgpack.h>

#include "gce_metadata.h"
#include "stackdriver.h"
#include "stackdriver_conf.h"
#include <mbedtls/base64.h>
#include <mbedtls/sha256.h>

/*
 * Base64 Encoding in JWT must:
 *
 * - remove any trailing padding '=' character
 * - replace '+' with '-'
 * - replace '/' with '_'
 *
 * ref: https://www.rfc-editor.org/rfc/rfc7515.txt Appendix C
 */
int jwt_base64_url_encode(unsigned char *out_buf, size_t out_size,
                          unsigned char *in_buf, size_t in_size,
                          size_t *olen)

{
    int i;
    size_t len;

    /* do normal base64 encoding */
    mbedtls_base64_encode(out_buf, out_size - 1,
                          &len, in_buf, in_size);

    /* Replace '+' and '/' characters */
    for (i = 0; i < len && out_buf[i] != '='; i++) {
        if (out_buf[i] == '+') {
            out_buf[i] = '-';
        }
        else if (out_buf[i] == '/') {
            out_buf[i] = '_';
        }
    }

    /* Now 'i' becomes the new length */
    *olen = i;
    return 0;
}


static int jwt_encode(char *payload, char *secret,
                      char **out_signature, size_t *out_size)
{
    int ret;
    int len;
    int buf_size;
    size_t olen;
    char *buf;
    char *sigd;
    char *headers = "{\"alg\": \"RS256\", \"typ\": \"JWT\"}";
    unsigned char sha256_buf[32] = {0};
    mbedtls_sha256_context sha256_ctx;
    mbedtls_rsa_context *rsa;
    flb_sds_t out;
    mbedtls_pk_context pk_ctx;
    unsigned char sig[256] = {0};

    buf_size = (strlen(payload) + strlen(secret)) * 2;
    buf = flb_malloc(buf_size);
    if (!buf) {
        flb_errno();
        return -1;
    }

    /* Encode header */
    len = strlen(headers);
    mbedtls_base64_encode((unsigned char *) buf, buf_size - 1,
                          &olen, (unsigned char *) headers, len);

    /* Create buffer to store JWT */
    out = flb_sds_create_size(2048);
    if (!out) {
        flb_errno();
        flb_free(buf);
        return -1;
    }

    /* Append header */
    flb_sds_cat(out, buf, olen);
    flb_sds_cat(out, ".", 1);

    /* Encode Payload */
    len = strlen(payload);
    jwt_base64_url_encode((unsigned char *) buf, buf_size,
                          (unsigned char *) payload, len, &olen);

    /* Append Payload */
    flb_sds_cat(out, buf, olen);

    /* do sha256() of base64(header).base64(payload) */
    mbedtls_sha256_init(&sha256_ctx);
    mbedtls_sha256_starts(&sha256_ctx, 0);
    mbedtls_sha256_update(&sha256_ctx, (const unsigned char *) out,
                          flb_sds_len(out));
    mbedtls_sha256_finish(&sha256_ctx, sha256_buf);

    /* In mbedTLS cert length must include the null byte */
    len = strlen(secret) + 1;

    /* Load Private Key */
    mbedtls_pk_init(&pk_ctx);
    ret = mbedtls_pk_parse_key(&pk_ctx,
                               (unsigned char *) secret, len, NULL, 0);
    if (ret != 0) {
        flb_error("[out_stackdriver] error loading private key");
        flb_free(buf);
        flb_sds_destroy(out);
        return -1;
    }

    /* Create RSA context */
    rsa = mbedtls_pk_rsa(pk_ctx);
    if (!rsa) {
        flb_error("[out_stackdriver] error creating RSA context");
        flb_free(buf);
        flb_sds_destroy(out);
        mbedtls_pk_free(&pk_ctx);
        return -1;
    }

    ret = mbedtls_rsa_pkcs1_sign(rsa, NULL, NULL,
                                 MBEDTLS_RSA_PRIVATE, MBEDTLS_MD_SHA256,
                                 0, (unsigned char *) sha256_buf, sig);
    if (ret != 0) {
        flb_error("[out_stackdriver] error signing SHA256");
        flb_free(buf);
        flb_sds_destroy(out);
        mbedtls_pk_free(&pk_ctx);
        return -1;
    }

    sigd = flb_malloc(2048);
    if (!sigd) {
        flb_errno();
        flb_free(buf);
        flb_sds_destroy(out);
        mbedtls_pk_free(&pk_ctx);
        return -1;
    }

    jwt_base64_url_encode((unsigned char *) sigd, 2048, sig, 256, &olen);

    flb_sds_cat(out, ".", 1);
    flb_sds_cat(out, sigd, olen);

    *out_signature = out;
    *out_size = flb_sds_len(out);

    flb_free(buf);
    flb_free(sigd);
    mbedtls_pk_free(&pk_ctx);

    return 0;
}

/* Create a new oauth2 context and get a oauth2 token */
static int get_oauth2_token(struct flb_stackdriver *ctx)
{
    int ret;
    char *token;
    char *sig_data;
    size_t sig_size;
    time_t issued;
    time_t expires;
    char payload[1024];

    /* Create oauth2 context */
    ctx->o = flb_oauth2_create(ctx->config, FLB_STD_AUTH_URL, 3000);
    if (!ctx->o) {
      flb_error("[out_stackdriver] cannot create oauth2 context");
      return -1;
    }

    /* In case of using metadata server, fetch token from there */
    if (ctx->metadata_server_auth) {
        return gce_metadata_read_token(ctx);
    }

    /* JWT encode for oauth2 */
    issued = time(NULL);
    expires = issued + FLB_STD_TOKEN_REFRESH;

    snprintf(payload, sizeof(payload) - 1,
             "{\"iss\": \"%s\", \"scope\": \"%s\", "
             "\"aud\": \"%s\", \"exp\": %lu, \"iat\": %lu}",
             ctx->client_email, FLB_STD_SCOPE,
             FLB_STD_AUTH_URL,
             expires, issued);

    /* Compose JWT signature */
    ret = jwt_encode(payload, ctx->private_key, &sig_data, &sig_size);
    if (ret != 0) {
        flb_error("[out_stackdriver] JWT signature generation failed");
        return -1;
    }
    flb_debug("[out_stackdriver] JWT signature:\n%s", sig_data);

    ret = flb_oauth2_payload_append(ctx->o,
                                    "grant_type", -1,
                                    "urn:ietf:params:oauth:"
                                    "grant-type:jwt-bearer", -1);
    if (ret == -1) {
        flb_error("[out_stackdriver] error appending oauth2 params");
        flb_sds_destroy(sig_data);
        return -1;
    }

    ret = flb_oauth2_payload_append(ctx->o,
                                    "assertion", -1,
                                    sig_data, sig_size);
    if (ret == -1) {
        flb_error("[out_stackdriver] error appending oauth2 params");
        flb_sds_destroy(sig_data);
        return -1;
    }
    flb_sds_destroy(sig_data);

    /* Retrieve access token */
    token = flb_oauth2_token_get(ctx->o);
    if (!token) {
        flb_error("[out_stackdriver] error retrieving oauth2 access token");
        return -1;
    }

    return 0;
}

static char *get_google_token(struct flb_stackdriver *ctx)
{
    int ret = 0;

    if (!ctx->o) {
        ret = get_oauth2_token(ctx);
    }
    else if (flb_oauth2_token_expired(ctx->o) == FLB_TRUE) {
        flb_oauth2_destroy(ctx->o);
        ret = get_oauth2_token(ctx);
    }

    if (ret != 0) {
        return NULL;
    }

    return ctx->o->access_token;
}

static int cb_stackdriver_init(struct flb_output_instance *ins,
                          struct flb_config *config, void *data)
{
    char *token;
    struct flb_stackdriver *ctx;
    int io_flags = FLB_IO_TLS;

    /* Create config context */
    ctx = flb_stackdriver_conf_create(ins, config);
    if (!ctx) {
        flb_error("[out_stackdriver] configuration failed");
        return -1;
    }

    /* Set context */
    flb_output_set_context(ins, ctx);

    /* Network mode IPv6 */
    if (ins->host.ipv6 == FLB_TRUE) {
        io_flags |= FLB_IO_IPV6;
    }

    /* Create Upstream context for Stackdriver Logging (no oauth2 service) */
    ctx->u = flb_upstream_create_url(config, FLB_STD_WRITE_URL,
                                     io_flags, &ins->tls);
    ctx->metadata_u = flb_upstream_create_url(config, "http://metadata.google.internal",
                                     FLB_IO_TCP, NULL);
    if (!ctx->u) {
        flb_error("[out_stackdriver] upstream creation failed");
        return -1;
    }
    if (!ctx->metadata_u) {
        flb_error("[out_stackdriver] metadata upstream creation failed");
        return -1;
    }

    /* Upstream Sync flags */
    ctx->u->flags &= ~FLB_IO_ASYNC;
    ctx->metadata_u->flags &= ~FLB_IO_ASYNC;

    /* Retrieve oauth2 token */
    token = get_google_token(ctx);
    if (!token) {
        flb_warn("[out_stackdriver] token retrieval failed");
    }
    if (ctx->metadata_server_auth) {
      gce_metadata_read_project_id(ctx);
      gce_metadata_read_zone(ctx);
      gce_metadata_read_instance_id(ctx);
    }
    return 0;
}

static int validate_severity_level(severity_t * s,
                                   const char * str,
                                   const unsigned int str_size)
{
    int i = 0;

    const static struct {
        severity_t s;
        const unsigned int str_size;
        const char * str;
    }   enum_mapping[] = {
        {EMERGENCY, 9, "EMERGENCY"},
        {EMERGENCY, 5, "EMERG"    },

        {ALERT    , 1, "A"        },
        {ALERT    , 5, "ALERT"    },

        {CRITICAL , 1, "C"        },
        {CRITICAL , 1, "F"        },
        {CRITICAL , 4, "CRIT"     },
        {CRITICAL , 5, "FATAL"    },
        {CRITICAL , 8, "CRITICAL" },

        {ERROR    , 1, "E"        },
        {ERROR    , 3, "ERR"      },
        {ERROR    , 5, "ERROR"    },
        {ERROR    , 6, "SEVERE"   },

        {WARNING  , 1, "W"        },
        {WARNING  , 4, "WARN"     },
        {WARNING  , 7, "WARNING"  },

        {NOTICE   , 1, "N"        },
        {NOTICE   , 6, "NOTICE"   },

        {INFO     , 1, "I"        },
        {INFO     , 4, "INFO"     },

        {DEBUG    , 1, "D"        },
        {DEBUG    , 5, "DEBUG"    },
        {DEBUG    , 5, "TRACE"    },
        {DEBUG    , 9, "TRACE_INT"},
        {DEBUG    , 4, "FINE"     },
        {DEBUG    , 5, "FINER"    },
        {DEBUG    , 6, "FINEST"   },
        {DEBUG    , 6, "CONFIG"   },

        {DEFAULT  , 7, "DEFAULT"  }
    };

    for (i = 0; i < sizeof (enum_mapping) / sizeof (enum_mapping[0]); ++i) {
        if (enum_mapping[i].str_size != str_size) {
            continue;
        }

        if (strncasecmp(str, enum_mapping[i].str, str_size) == 0) {
            *s = enum_mapping[i].s;
            return 0;
        }
    }
    return -1;
}

static int get_msgpack_obj(msgpack_object * subobj, const msgpack_object * o,
                           const flb_sds_t key, const int key_size,
                           msgpack_object_type type)
{
    int i = 0;
    msgpack_object_kv * p = NULL;

    if (o == NULL || subobj == NULL) {
        return -1;
    }

    for (i = 0; i < o->via.map.size; i++) {
        p = &o->via.map.ptr[i];
        if (p->val.type != type) {
            continue;
        }

        if (flb_sds_cmp(key, p->key.via.str.ptr, p->key.via.str.size) == 0) {
            *subobj = p->val;
            return 0;
        }
    }
    return -1;
}

static int get_severity_level(severity_t * s, const msgpack_object * o,
                              const flb_sds_t key)
{
    msgpack_object tmp;
    if (get_msgpack_obj(&tmp, o, key, flb_sds_len(key), MSGPACK_OBJECT_STR) == 0
        && validate_severity_level(s, tmp.via.str.ptr, tmp.via.str.size) == 0) {
        return 0;
    }
    *s = 0;
    return -1;
}

static int get_string(msgpack_object_str *str, const msgpack_object * o, const flb_sds_t key)
{
    msgpack_object tmp;
    if (get_msgpack_obj(&tmp, o, key, flb_sds_len(key), MSGPACK_OBJECT_STR) == 0) {
        *str = tmp.via.str;
        return 0;
    }
    *str = (const msgpack_object_str){0};
    return -1;
}

static int get_map(msgpack_object *map, const msgpack_object * o,
                   const flb_sds_t key)
{
    if (get_msgpack_obj(map, o, key, flb_sds_len(key), MSGPACK_OBJECT_MAP) == 0) {
        return 0;
    }
    *map = (const msgpack_object){0};
    return -1;
}

static void log_msgpack_obj(int verbose, const msgpack_object *obj, const char *key)
{
    if (verbose < 4) {
        return;
    }
    char *bp;
    size_t size;
    FILE *stream;
    stream = open_memstream(&bp, &size);
    msgpack_object_print(stream, *obj);
    fclose(stream);
    flb_debug("[out_stackdriver] %s: %s", key, bp);
    free(bp);
}

static void msgpack_string_kv(msgpack_packer *mp_pck, const char *label, size_t label_size,
                              const char *value, size_t value_size)
{
    msgpack_pack_str(mp_pck, label_size);
    msgpack_pack_str_body(mp_pck, label, label_size);
    msgpack_pack_str(mp_pck, value_size);
    msgpack_pack_str_body(mp_pck, value, value_size);
}

static void msgpack_object_string_kv(msgpack_packer *mp_pck, const char *label, size_t label_size,
                                     const msgpack_object_str *value)
{
    msgpack_pack_str(mp_pck, label_size);
    msgpack_pack_str_body(mp_pck, label, label_size);
    msgpack_pack_str(mp_pck, value->size);
    msgpack_pack_str_body(mp_pck, value->ptr, value->size);
}

static int stackdriver_format(const void *data, size_t bytes,
                              const char *tag, size_t tag_len,
                              char **out_data, size_t *out_size,
                              struct flb_stackdriver *ctx,
                              int verbose)
{
    int len;
    int array_size = 0;
    size_t s;
    size_t off = 0;
    char path[PATH_MAX];
    char time_formatted[255];
    struct tm tm;
    struct flb_time tms;
    severity_t severity;
    msgpack_object_str trace;
    msgpack_object_str cluster_name;
    msgpack_object_str container_name;
    msgpack_object_str namespace_id;
    msgpack_object_str pod_id;
    msgpack_object_str stacktrace;
    msgpack_object_str message;
    msgpack_object_str service;
    msgpack_object_str version;
    msgpack_object *obj;
    msgpack_object json_payload;
    msgpack_object kubernetes;
    msgpack_object kubernetes_labels;
    msgpack_object http_request;
    int i;
    flb_sds_t new_message;
    msgpack_object_kv *p;
    msgpack_unpacked result;
    msgpack_sbuffer mp_sbuf;
    msgpack_packer mp_pck;
    flb_sds_t out_buf;

    /* Count number of records */
    msgpack_unpacked_init(&result);
    while (msgpack_unpack_next(&result, data, bytes, &off) == MSGPACK_UNPACK_SUCCESS) {
        array_size++;
    }
    msgpack_unpacked_destroy(&result);
    msgpack_unpacked_init(&result);

    /* Create temporal msgpack buffer */
    msgpack_sbuffer_init(&mp_sbuf);
    msgpack_packer_init(&mp_pck, &mp_sbuf, msgpack_sbuffer_write);

    /*
     * Pack root map (resource & entries):
     *
     * {"resource": {"type": "...", "labels": {...},
     *  "entries": []
     */
    if (strcmp(ctx->resource, "container") == 0) {
        msgpack_pack_map(&mp_pck, 1);
    }
    else {
        msgpack_pack_map(&mp_pck, 2);

        msgpack_pack_str(&mp_pck, 8);
        msgpack_pack_str_body(&mp_pck, "resource", 8);

        /* type & labels */
        msgpack_pack_map(&mp_pck, 2);

        /* type */
        msgpack_pack_str(&mp_pck, 4);
        msgpack_pack_str_body(&mp_pck, "type", 4);
        msgpack_pack_str(&mp_pck, flb_sds_len(ctx->resource));
        msgpack_pack_str_body(&mp_pck, ctx->resource,
                              flb_sds_len(ctx->resource));

        msgpack_pack_str(&mp_pck, 6);
        msgpack_pack_str_body(&mp_pck, "labels", 6);

        if (strcmp(ctx->resource, "global") == 0) {
          /* global resource has field project_id */
          msgpack_pack_map(&mp_pck, 1);
          msgpack_pack_str(&mp_pck, 10);
          msgpack_pack_str_body(&mp_pck, "project_id", 10);
          msgpack_pack_str(&mp_pck, flb_sds_len(ctx->project_id));
          msgpack_pack_str_body(&mp_pck,
                                ctx->project_id, flb_sds_len(ctx->project_id));
        } else if (strcmp(ctx->resource, "gce_instance") == 0) {
          /* gce_instance resource has fields project_id, zone, instance_id */
          msgpack_pack_map(&mp_pck, 3);

          msgpack_pack_str(&mp_pck, 10);
          msgpack_pack_str_body(&mp_pck, "project_id", 10);
          msgpack_pack_str(&mp_pck, flb_sds_len(ctx->project_id));
          msgpack_pack_str_body(&mp_pck,
                                ctx->project_id, flb_sds_len(ctx->project_id));

          msgpack_pack_str(&mp_pck, 4);
          msgpack_pack_str_body(&mp_pck, "zone", 4);
          msgpack_pack_str(&mp_pck, flb_sds_len(ctx->zone));
          msgpack_pack_str_body(&mp_pck, ctx->zone, flb_sds_len(ctx->zone));

          msgpack_pack_str(&mp_pck, 11);
          msgpack_pack_str_body(&mp_pck, "instance_id", 11);
          msgpack_pack_str(&mp_pck, flb_sds_len(ctx->instance_id));
          msgpack_pack_str_body(&mp_pck,
                                ctx->instance_id, flb_sds_len(ctx->instance_id));
        }
    }

    msgpack_pack_str(&mp_pck, 7);
    msgpack_pack_str_body(&mp_pck, "entries", 7);

    /* Append entries */
    msgpack_pack_array(&mp_pck, array_size);

    off = 0;
    while (msgpack_unpack_next(&result, data, bytes, &off) == MSGPACK_UNPACK_SUCCESS) {
        severity = 0;
        trace = (const msgpack_object_str){0};
        cluster_name = (const msgpack_object_str){0};
        container_name = (const msgpack_object_str){0};
        namespace_id = (const msgpack_object_str){0};
        pod_id = (const msgpack_object_str){0};
        stacktrace = (const msgpack_object_str){0};
        message = (const msgpack_object_str){0};
        json_payload = (const msgpack_object){0};
        kubernetes = (const msgpack_object){0};
        kubernetes_labels = (const msgpack_object){0};
        http_request = (const msgpack_object){0};
        new_message = NULL;

        /* Get timestamp */
        flb_time_pop_from_msgpack(&tms, &result, &obj);

        /*
         * Pack entry
         *
         * {
         *  "severity": "...",
         *  "trace": "...",
         *  "logName": "...",
         *  "jsonPayload": {...},
         *  "resources": {...},
         *  "labels": {...},
         *  "timestamp": "..."
         * }
         */
        int entries = 3;

        if (ctx->severity_key
            && get_severity_level(&severity, obj, ctx->severity_key) == 0) {
            ++entries;
        }

        if (ctx->trace_key
            && get_string(&trace, obj, ctx->trace_key) == 0) {
            ++entries;
        }

        if (ctx->kubernetes_key
            && get_map(&kubernetes, obj, ctx->kubernetes_key) == 0) {
            ++entries;

            if (get_map(&kubernetes_labels, &kubernetes, ctx->labels_key) == 0) {
                ++entries;
            }
        }

        if (ctx->http_request_key
            && get_map(&http_request, obj, ctx->http_request_key) == 0) {
            ++entries;
        }

        msgpack_pack_map(&mp_pck, entries);

        if (severity) {
            /* additional field for severity */
            msgpack_pack_str(&mp_pck, 8);
            msgpack_pack_str_body(&mp_pck, "severity", 8);
            msgpack_pack_int(&mp_pck, severity);
        }

        if (trace.ptr != NULL) {
            msgpack_object_string_kv(&mp_pck, "trace", 5, &trace);
        }

        /* Get jsonPayload */
        if (!ctx->json_key
            || get_map(&json_payload, obj, ctx->json_key) != 0) {
            json_payload = *obj;
        }

        /* Concatentate message with stacktrace if there's a separate stacktrace key. */
        if (ctx->stacktrace_key
            && get_string(&stacktrace, &json_payload, ctx->stacktrace_key) == 0
            && get_string(&message, &json_payload, ctx->message_key) == 0) {

            size_t size;
            FILE *stream = open_memstream(&new_message, &size);
            fwrite(message.ptr, message.size, 1, stream);
            fwrite(" ", 1, 1, stream);
            fwrite(stacktrace.ptr, stacktrace.size, 1, stream);
            fwrite("\0", 1, 1, stream);
            fclose(stream);
            flb_debug("[out_stackdriver] new message: %s", new_message);
        }

        /* Write jsonPayload. */
        msgpack_pack_str(&mp_pck, 11);
        msgpack_pack_str_body(&mp_pck, "jsonPayload", 11);
        if (new_message != NULL) {
            msgpack_pack_map(&mp_pck, json_payload.via.map.size);

            /* Write the serviceContext to enable error reporting */
            msgpack_pack_str(&mp_pck, 14);
            msgpack_pack_str_body(&mp_pck, "serviceContext", 14);
            msgpack_pack_map(&mp_pck, 2);
            msgpack_pack_str(&mp_pck, 7);
            msgpack_pack_str_body(&mp_pck, "service", 7);
            if (get_string(&service, &kubernetes_labels, ctx->service_label) == 0) {
                msgpack_pack_str(&mp_pck, service.size);
                msgpack_pack_str_body(&mp_pck, service.ptr, service.size);
            }
            else {
                msgpack_pack_str(&mp_pck, 5);
                msgpack_pack_str_body(&mp_pck, "other", 5);
            }
            msgpack_pack_str(&mp_pck, 7);
            msgpack_pack_str_body(&mp_pck, "version", 7);
            if (get_string(&version, &kubernetes_labels, ctx->version_label) == 0) {
                msgpack_pack_str(&mp_pck, version.size);
                msgpack_pack_str_body(&mp_pck, version.ptr, version.size);
            }
            else {
                msgpack_pack_str(&mp_pck, 5);
                msgpack_pack_str_body(&mp_pck, "other", 5);
            }

            for (i = 0; i < json_payload.via.map.size; i++) {
                p = &json_payload.via.map.ptr[i];
                if (flb_sds_cmp(ctx->message_key, p->key.via.str.ptr, p->key.via.str.size) == 0) {
                    msgpack_pack_object(&mp_pck, p->key);
                    msgpack_pack_str(&mp_pck, strlen(new_message));
                    msgpack_pack_str_body(&mp_pck, new_message, strlen(new_message));
                }
                else if (flb_sds_cmp(
                    ctx->stacktrace_key, p->key.via.str.ptr, p->key.via.str.size) == 0) {
                    /* skip the stacktrace */
                }
                else {
                    msgpack_pack_object(&mp_pck, p->key);
                    msgpack_pack_object(&mp_pck, p->val);
                }
            }
            free(new_message);
        }
        else {
            msgpack_pack_object(&mp_pck, json_payload);
        }

        /* resource labels */
        if (kubernetes.type != 0) {
            log_msgpack_obj(verbose, &kubernetes, "kubernetes");

            msgpack_pack_str(&mp_pck, 8);
            msgpack_pack_str_body(&mp_pck, "resource", 8);
            msgpack_pack_map(&mp_pck, 2);
            msgpack_string_kv(&mp_pck, "type", 4, "container", 9);
            msgpack_pack_str(&mp_pck, 6);
            msgpack_pack_str_body(&mp_pck, "labels", 6);

            int labels = 3;

            if (get_string(&cluster_name, &kubernetes, ctx->cluster_name_key) == 0) {
                ++labels;
            }

            if (get_string(&container_name, &kubernetes, ctx->container_name_key) == 0) {
                ++labels;
            }

            if (get_string(&namespace_id, &kubernetes, ctx->namespace_id_key) == 0) {
                ++labels;
            }

            if (get_string(&pod_id, &kubernetes, ctx->pod_id_key) == 0) {
                ++labels;
            }

            msgpack_pack_map(&mp_pck, labels);

            msgpack_string_kv(&mp_pck, "project_id", 10,
                              ctx->project_id, flb_sds_len(ctx->project_id));
            msgpack_string_kv(&mp_pck, "zone", 4,
                              ctx->zone, flb_sds_len(ctx->zone));
            msgpack_string_kv(&mp_pck, "instance_id", 11,
                              ctx->instance_id, flb_sds_len(ctx->instance_id));

            if (cluster_name.ptr != NULL) {
                msgpack_object_string_kv(&mp_pck, "cluster_name", 12, &cluster_name);
            }

            if (container_name.ptr != NULL) {
                msgpack_object_string_kv(&mp_pck, "container_name", 14, &container_name);
            }

            if (namespace_id.ptr != NULL) {
                msgpack_object_string_kv(&mp_pck, "namespace_id", 12, &namespace_id);
            }

            if (pod_id.ptr != NULL) {
                msgpack_object_string_kv(&mp_pck, "pod_id", 6, &pod_id);
            }
        }

        /* additional labels from Kubernetes */
        if (kubernetes_labels.type != 0) {
            log_msgpack_obj(verbose, &kubernetes_labels, "labels");

            msgpack_pack_str(&mp_pck, 6);
            msgpack_pack_str_body(&mp_pck, "labels", 6);
            msgpack_pack_object(&mp_pck, kubernetes_labels);
        }

        /* HTTP request */
        if (http_request.type != 0) {
            log_msgpack_obj(verbose, &kubernetes_labels, "httpRequest");

            msgpack_pack_str(&mp_pck, 11);
            msgpack_pack_str_body(&mp_pck, "httpRequest", 11);
            msgpack_pack_object(&mp_pck, http_request);
        }

        /* logName */
        len = snprintf(path, sizeof(path) - 1,
                       "projects/%s/logs/%s", ctx->project_id, tag);

        msgpack_pack_str(&mp_pck, 7);
        msgpack_pack_str_body(&mp_pck, "logName", 7);
        msgpack_pack_str(&mp_pck, len);
        msgpack_pack_str_body(&mp_pck, path, len);

        /* timestamp */
        msgpack_pack_str(&mp_pck, 9);
        msgpack_pack_str_body(&mp_pck, "timestamp", 9);

        /* Format the time */
        gmtime_r(&tms.tm.tv_sec, &tm);
        s = strftime(time_formatted, sizeof(time_formatted) - 1,
                     FLB_STD_TIME_FMT, &tm);
        len = snprintf(time_formatted + s, sizeof(time_formatted) - 1 - s,
                       ".%09" PRIu64 "Z", (uint64_t) tms.tm.tv_nsec);
        s += len;

        msgpack_pack_str(&mp_pck, s);
        msgpack_pack_str_body(&mp_pck, time_formatted, s);
    }

    /* Convert from msgpack to JSON */
    out_buf = flb_msgpack_raw_to_json_sds(mp_sbuf.data, mp_sbuf.size);
    msgpack_sbuffer_destroy(&mp_sbuf);

    if (!out_buf) {
        flb_error("[out_stackdriver] error formatting JSON payload");
        msgpack_unpacked_destroy(&result);
        return -1;
    }

    *out_data = out_buf;
    *out_size = flb_sds_len(out_buf);

    flb_debug("[out_stackdriver] output data: %s", *out_data);

    return 0;
}

static void set_authorization_header(struct flb_http_client *c,
                                     char *token)
{
    int len;
    char header[512];

    len = snprintf(header, sizeof(header) - 1,
                   "Bearer %s", token);
    flb_http_add_header(c, "Authorization", 13, header, len);
}

static void cb_stackdriver_flush(const void *data, size_t bytes,
                                 const char *tag, int tag_len,
                                 struct flb_input_instance *i_ins,
                                 void *out_context,
                                 struct flb_config *config)
{
    (void) i_ins;
    (void) config;
    int ret;
    int ret_code = FLB_RETRY;
    size_t b_sent;
    char *token;
    flb_sds_t payload_buf;
    size_t payload_size;
    struct flb_stackdriver *ctx = out_context;
    struct flb_upstream_conn *u_conn;
    struct flb_http_client *c;

    /* Get upstream connection */
    u_conn = flb_upstream_conn_get(ctx->u);
    if (!u_conn) {
        FLB_OUTPUT_RETURN(FLB_RETRY);
    }

    /* Reformat msgpack to stackdriver JSON payload */
    ret = stackdriver_format(data, bytes, tag, tag_len,
                             &payload_buf, &payload_size, ctx,
                             config->verbose);
    if (ret != 0) {
        flb_upstream_conn_release(u_conn);
        FLB_OUTPUT_RETURN(FLB_RETRY);
    }

    /* Get or renew Token */
    token = get_google_token(ctx);
    if (!token) {
        flb_error("[out_stackdriver] cannot retrieve oauth2 token");
        flb_upstream_conn_release(u_conn);
        flb_sds_destroy(payload_buf);
        FLB_OUTPUT_RETURN(FLB_RETRY);
    }

    /* Compose HTTP Client request */
    c = flb_http_client(u_conn, FLB_HTTP_POST, FLB_STD_WRITE_URI,
                        payload_buf, payload_size, NULL, 0, NULL, 0);

    flb_http_buffer_size(c, 4192);

    flb_http_add_header(c, "User-Agent", 10, "Fluent-Bit", 10);
    flb_http_add_header(c, "Content-Type", 12, "application/json", 16);

    /* Compose and append Authorization header */
    set_authorization_header(c, token);

    /* Send HTTP request */
    ret = flb_http_do(c, &b_sent);

    /* validate response */
    if (ret != 0) {
        flb_warn("[out_stackdriver] http_do=%i", ret);
        ret_code = FLB_RETRY;
    }
    else {
        /* The request was issued successfully, validate the 'error' field */
        flb_debug("[out_stackdriver] HTTP Status=%i", c->resp.status);
        if (c->resp.status == 200) {
            ret_code = FLB_OK;
        }
        else {
            if (c->resp.payload_size > 0) {
                /* we got an error */
                flb_warn("[out_stackdriver] error\n%s",
                         c->resp.payload);
            }
            else {
                flb_debug("[out_stackdriver] response\n%s",
                          c->resp.payload);
            }
            ret_code = FLB_RETRY;
        }
    }

    /* Cleanup */
    flb_sds_destroy(payload_buf);
    flb_http_client_destroy(c);
    flb_upstream_conn_release(u_conn);

    /* Done */
    FLB_OUTPUT_RETURN(ret_code);
}

static int cb_stackdriver_exit(void *data, struct flb_config *config)
{
    struct flb_stackdriver *ctx = data;

    if (!ctx) {
        return -1;
    }

    if (ctx->u) {
        flb_upstream_destroy(ctx->u);
    }

    flb_stackdriver_conf_destroy(ctx);
    return 0;
}

struct flb_output_plugin out_stackdriver_plugin = {
    .name         = "stackdriver",
    .description  = "Send events to Google Stackdriver Logging",
    .cb_init      = cb_stackdriver_init,
    .cb_flush     = cb_stackdriver_flush,
    .cb_exit      = cb_stackdriver_exit,

    /* Plugin flags */
    .flags          = FLB_OUTPUT_NET | FLB_IO_TLS,
};
