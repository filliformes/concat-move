/**
 * Concat — audio ingest (clean-room MIT; decode via vendored dr_libs, public domain).
 *
 * Folder discovery + decode of WAV/FLAC/MP3 to mono float @ a target sample rate
 * (downmix + cubic resample). Offline (analyst/scan thread) only — uses stdio,
 * opendir, malloc; never call from render.
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

/* Scan the standard on-device roots for folders containing audio (WAV/FLAC/MP3).
 * Writes up to `max` entries into `out`; returns the count. */
int ingest_scan_folders(corpus_folder_t *out, int max);

/* List absolute paths of audio files directly inside `dir`. Returns count (<= max). */
int ingest_list_audio(const char *dir, char (*paths)[INGEST_PATH], int max);

/* Decode a file to mono float @ target_sr. Returns a malloc'd buffer of *out_len
 * frames (caller frees with free()), or NULL on failure. */
float *ingest_decode(const char *path, int target_sr, int *out_len);

#endif /* CONCAT_INGEST_H */
