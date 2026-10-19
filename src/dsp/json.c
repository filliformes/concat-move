/**
 * Concat — minimal JSON reader (clean-room, first-party MIT). See json.h.
 */
#include "json.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define JSON_MAX_DEPTH 32

typedef struct { const char *p; char *err; int errlen; } jp_t;

static void fail(jp_t *j, const char *msg) {
    if (j->err && j->errlen > 0 && !j->err[0]) snprintf(j->err, j->errlen, "%s", msg);
}

static void skip_ws(jp_t *j) {
    while (*j->p == ' ' || *j->p == '\t' || *j->p == '\n' || *j->p == '\r') j->p++;
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int put_utf8(char *o, unsigned cp) {
    if (cp < 0x80)    { o[0] = (char)cp; return 1; }
    if (cp < 0x800)   { o[0] = (char)(0xC0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) { o[0] = (char)(0xE0 | (cp >> 12)); o[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
                        o[2] = (char)(0x80 | (cp & 0x3F)); return 3; }
    o[0] = (char)(0xF0 | (cp >> 18)); o[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    o[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); o[3] = (char)(0x80 | (cp & 0x3F)); return 4;
}

static int read_hex4(const char *s, unsigned *out) {
    unsigned v = 0;
    for (int i = 0; i < 4; i++) { int h = hexval(s[i]); if (h < 0) return 0; v = (v << 4) | (unsigned)h; }
    *out = v; return 1;
}

/* Parse a string starting at the opening quote. The decoded text is never longer
 * than the source, so one allocation of the source span is enough. */
static char *parse_string(jp_t *j) {
    const char *s = ++j->p, *e = s;
    while (*e && *e != '"') { if (*e == '\\' && e[1]) e++; e++; }
    if (*e != '"') { fail(j, "unterminated string"); return NULL; }
    char *out = (char *)malloc((size_t)(e - s) + 1), *o = out;
    if (!out) { fail(j, "out of memory"); return NULL; }
    while (j->p < e) {
        char c = *j->p++;
        if (c != '\\') { *o++ = c; continue; }
        c = *j->p++;
        switch (c) {
            case '"': case '\\': case '/': *o++ = c; break;
            case 'b': *o++ = '\b'; break;
            case 'f': *o++ = '\f'; break;
            case 'n': *o++ = '\n'; break;
            case 'r': *o++ = '\r'; break;
            case 't': *o++ = '\t'; break;
            case 'u': {
                unsigned cp;
                if (e - j->p < 4 || !read_hex4(j->p, &cp)) { free(out); fail(j, "bad \\u escape"); return NULL; }
                j->p += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF && e - j->p >= 6 && j->p[0] == '\\' && j->p[1] == 'u') {
                    unsigned lo;
                    if (read_hex4(j->p + 2, &lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        j->p += 6;
                    }
                }
                o += put_utf8(o, cp);   /* \uXXXX is 6 source bytes, UTF-8 output is at most 4 */
                break;
            }
            default: free(out); fail(j, "bad escape"); return NULL;
        }
    }
    *o = 0;
    j->p = e + 1;
    return out;
}

static jv_t *parse_value(jp_t *j, int depth);

static jv_t *new_node(jp_t *j, int type) {
    jv_t *v = (jv_t *)calloc(1, sizeof(jv_t));
    if (!v) fail(j, "out of memory");
    else v->type = type;
    return v;
}

static jv_t *parse_container(jp_t *j, int depth, int obj) {
    if (depth > JSON_MAX_DEPTH) { fail(j, "nested too deep"); return NULL; }
    jv_t *v = new_node(j, obj ? JV_OBJ : JV_ARR);
    if (!v) return NULL;
    char close = obj ? '}' : ']';
    j->p++;
    skip_ws(j);
    if (*j->p == close) { j->p++; return v; }
    jv_t **tail = &v->child;
    for (;;) {
        char *key = NULL;
        skip_ws(j);
        if (obj) {
            if (*j->p != '"') { fail(j, "expected a member name"); json_free(v); return NULL; }
            key = parse_string(j);
            if (!key) { json_free(v); return NULL; }
            skip_ws(j);
            if (*j->p != ':') { free(key); fail(j, "expected ':'"); json_free(v); return NULL; }
            j->p++;
        }
        jv_t *c = parse_value(j, depth + 1);
        if (!c) { free(key); json_free(v); return NULL; }
        c->key = key;
        *tail = c; tail = &c->next;
        skip_ws(j);
        if (*j->p == ',') { j->p++; continue; }
        if (*j->p == close) { j->p++; return v; }
        fail(j, obj ? "expected ',' or '}'" : "expected ',' or ']'");
        json_free(v);
        return NULL;
    }
}

static jv_t *parse_value(jp_t *j, int depth) {
    skip_ws(j);
    char c = *j->p;
    if (c == '{') return parse_container(j, depth, 1);
    if (c == '[') return parse_container(j, depth, 0);
    if (c == '"') {
        char *s = parse_string(j);
        if (!s) return NULL;
        jv_t *v = new_node(j, JV_STR);
        if (!v) { free(s); return NULL; }
        v->str = s;
        return v;
    }
    if (!strncmp(j->p, "true", 4))  { j->p += 4; jv_t *v = new_node(j, JV_BOOL); if (v) v->num = 1; return v; }
    if (!strncmp(j->p, "false", 5)) { j->p += 5; return new_node(j, JV_BOOL); }
    if (!strncmp(j->p, "null", 4))  { j->p += 4; return new_node(j, JV_NULL); }
    if (c == '-' || (c >= '0' && c <= '9')) {
        char *end;
        double d = strtod(j->p, &end);
        if (end == j->p) { fail(j, "bad number"); return NULL; }
        j->p = end;
        jv_t *v = new_node(j, JV_NUM);
        if (v) v->num = d;
        return v;
    }
    fail(j, c ? "unexpected character" : "unexpected end");
    return NULL;
}

jv_t *json_parse(const char *text, char *err, int errlen) {
    if (err && errlen > 0) err[0] = 0;
    if (!text) return NULL;
    jp_t j = { text, err, errlen };
    if ((unsigned char)j.p[0] == 0xEF && (unsigned char)j.p[1] == 0xBB && (unsigned char)j.p[2] == 0xBF) j.p += 3;  /* BOM */
    jv_t *v = parse_value(&j, 0);
    if (!v) return NULL;
    skip_ws(&j);
    if (*j.p) { fail(&j, "trailing characters"); json_free(v); return NULL; }
    return v;
}

void json_free(jv_t *v) {
    while (v) {
        jv_t *n = v->next;
        json_free(v->child);
        free(v->str); free(v->key); free(v);
        v = n;
    }
}

const jv_t *json_get(const jv_t *obj, const char *key) {
    if (!obj || obj->type != JV_OBJ || !key) return NULL;
    for (const jv_t *c = obj->child; c; c = c->next)
        if (c->key && !strcmp(c->key, key)) return c;
    return NULL;
}

double json_num(const jv_t *v, double dflt)            { return (v && v->type == JV_NUM) ? v->num : dflt; }
const char *json_str(const jv_t *v, const char *dflt)  { return (v && v->type == JV_STR) ? v->str : dflt; }
int json_bool(const jv_t *v, int dflt)                 { return (v && v->type == JV_BOOL) ? (int)v->num : dflt; }
