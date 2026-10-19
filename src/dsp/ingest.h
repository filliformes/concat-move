/**
 * Concat — audio ingest (clean-room MIT; decode via vendored dr_libs, public domain).
 *
 * Folder discovery + decode of WAV/AIFF/FLAC/MP3 to mono float @ a target sample
 * rate (downmix + cubic resample). Offline (analyst/scan thread) only — uses stdio,
 * opendir, malloc; never call from render.
 *
 * Decoding is STREAMED: a reader yields mono frames at the target rate in chunks, so
 * memory stays bounded whatever the file length (the old whole-file decode needed
 * ~1 GB for a 30-minute MP3).
 */
#ifndef CONCAT_INGEST_H
#define CONCAT_INGEST_H

#define INGEST_MAX_FOLDERS 96
#define INGEST_PATH        512

typedef struct {
    char name[80];              /* short display name (folder basename) */
    char path[INGEST_PATH];     /* absolute path */
    int  nfiles;                /* audio files directly inside */
} corpus_folder_t;

/* 1 if the file name has an extension Concat decodes (.wav .aif .aiff .flac .mp3). */
int ingest_is_audio(const char *name);

/* Scan the standard on-device roots for folders containing audio.
 * Writes up to `max` entries into `out`; returns the count. */
int ingest_scan_folders(corpus_folder_t *out, int max);

/* List absolute paths of audio files inside `dir` (recursive, bounded depth), or the
 * file itself when `dir` is a regular file. Returns count (<= max). */
int ingest_list_audio(const char *dir, char (*paths)[INGEST_PATH], int max);
/* Same, with recursion optional (0 = only files directly inside `dir`). */
int ingest_list_audio_ex(const char *dir, char (*paths)[INGEST_PATH], int max, int recurse);

/* ── Streaming reader ─────────────────────────────────────────────────────── */
typedef struct ingest_reader ingest_reader_t;

/* Open a file for streamed mono decode at target_sr. NULL if it cannot be decoded. */
ingest_reader_t *ingest_open(const char *path, int target_sr);

/* Read up to max_frames mono frames at the target rate into out.
 * Returns frames written; 0 at end of file. */
int ingest_read(ingest_reader_t *r, float *out, int max_frames);

/* Position the reader so the next ingest_read returns output (target-rate) frame
 * `frame`. Returns 0 if the decoder cannot seek there. */
int ingest_seek(ingest_reader_t *r, long long frame);

/* Source duration in seconds (0 if unknown, e.g. some MP3s). */
double ingest_duration_s(const ingest_reader_t *r);

void ingest_close(ingest_reader_t *r);

/* Convenience (tests, small files): decode a whole file through the reader.
 * Returns a malloc'd buffer of *out_len frames (caller frees), or NULL on failure. */
float *ingest_decode(const char *path, int target_sr, int *out_len);

#endif /* CONCAT_INGEST_H */
