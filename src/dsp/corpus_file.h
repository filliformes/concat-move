/**
 * Concat — corpus sources: folder, single file, or a .concat corpus file
 * (clean-room, first-party MIT). Analyst thread only (stat, opendir, malloc).
 *
 * A .concat file is JSON (see docs/corpus-builder-design.md):
 *   {
 *     "concat": 1,
 *     "name": "Night Market",
 *     "sources": [
 *       { "path": "Samples/Ahuntsic_Ambiances" },
 *       { "path": "Samples/MELLOTRON/M300SaxB-20.wav", "start": 0.4, "end": 6.2 },
 *       { "path": "Samples/DRUMS", "include": ["*Kick*"], "recurse": false }
 *     ],
 *     "exclude": ["*click*"],
 *     "descriptors": { "use": ["loud", "centroid", "flatness", "pitch", "mfcc1-13"] },
 *     "slicing": { "mode": "onset", "min_ms": 40, "max_ms": 600 }
 *   }
 * Relative paths are under /data/UserData/UserLibrary. Globs match file names, case
 * insensitive. Top-level "exclude" applies to folder sources. A missing source is
 * skipped and counted, never fatal.
 */
#ifndef CONCAT_CORPUS_FILE_H
#define CONCAT_CORPUS_FILE_H

#include "ingest.h"
#include "corpus.h"   /* NDESC */

#define CF_LIBRARY_ROOT "/data/UserData/UserLibrary"
#define CF_MAX_WATCH    32

/* One audio file (or a range of one) that enters the corpus. */
typedef struct {
    char   path[INGEST_PATH];
    double start_s, end_s;     /* seconds; end_s <= start_s means "to the end of the file" */
    int    src;                /* index of its entry in "sources" (0 for a folder/file pick) */
} corpus_item_t;

typedef struct {
    int    is_concat;
    char   name[64];
    int    has_slicing;        /* the file overrides the Corpus-page slicing knobs */
    int    slice_mode;         /* 0 onset, 1 fixed */
    int    min_ms, max_ms;
    int    has_mask;           /* descriptors.use given */
    float  mask[NDESC];        /* 1 = descriptor used, 0 = ignored (zeroed in query space) */
    int    unknown_desc;       /* names in descriptors.use that matched nothing */
    int    sources_total, sources_missing;
    int    nwatch;             /* what hot reload stats: the .concat itself + folder sources */
    char   watch[CF_MAX_WATCH][INGEST_PATH];
    char   err[96];            /* why a .concat could not be read (empty if fine) */
} corpus_spec_t;

/* 1 if the path names a .concat file (by extension). */
int corpus_is_concat(const char *path);

/* Resolve `path` (folder, audio file or .concat) into items. Returns the item count,
 * 0 when nothing resolves, -1 when a .concat cannot be read or parsed (spec->err). */
int corpus_resolve(const char *path, corpus_spec_t *spec, corpus_item_t *items, int max);

/* Descriptor group name -> mask bits ("loud", "centroid", "flatness", "zcr", "pitch",
 * "mfcc", "mfccN", "mfccA-B"; each takes mean and std). Returns 0 if unknown. */
int corpus_desc_group(const char *name, float *mask);

/* Change stamp over the watched paths (mtime + size). 0 if none can be stat'ed. */
unsigned long long corpus_watch_stamp(const corpus_spec_t *spec);

#endif /* CONCAT_CORPUS_FILE_H */
