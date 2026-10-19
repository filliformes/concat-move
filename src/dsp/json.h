/**
 * Concat — minimal JSON reader (clean-room, first-party MIT).
 *
 * Parses a document into a malloc'd tree for reading .concat corpus files on the
 * analyst thread. Strict enough for hand- and tool-written files: objects, arrays,
 * strings (with \uXXXX escapes, encoded back to UTF-8), numbers, true/false/null.
 * Never call from render.
 */
#ifndef CONCAT_JSON_H
#define CONCAT_JSON_H

enum { JV_NULL = 0, JV_BOOL, JV_NUM, JV_STR, JV_ARR, JV_OBJ };

typedef struct jv {
    int        type;
    double     num;          /* JV_NUM, JV_BOOL (0/1) */
    char      *str;          /* JV_STR */
    char      *key;          /* member name when the parent is an object */
    struct jv *child;        /* first element / member (JV_ARR, JV_OBJ) */
    struct jv *next;         /* next sibling */
} jv_t;

/* Parse `text`. Returns the root, or NULL with a short reason in err. */
jv_t *json_parse(const char *text, char *err, int errlen);
void  json_free(jv_t *v);

/* Object member by name (NULL if absent or `obj` is not an object). */
const jv_t *json_get(const jv_t *obj, const char *key);

/* Typed reads with a fallback when the value is absent or of another type. */
double      json_num(const jv_t *v, double dflt);
const char *json_str(const jv_t *v, const char *dflt);
int         json_bool(const jv_t *v, int dflt);

#endif /* CONCAT_JSON_H */
