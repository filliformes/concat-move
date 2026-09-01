/**
 * Concat — corpus descriptor layer (clean-room, first-party MIT).
 *
 * Turns audio into the per-slice descriptor vectors the query navigates:
 *   1. onset slicing (spectral flux) → slice boundaries
 *   2. per-slice descriptor = MultiStats (mean+std) of per-frame descriptors
 *   3. RobustScaling (median/IQR) normalisation for the query distance
 *
 * Offline (analyst thread) only. Depends on analysis.{h,c}. No render-path use.
 */
#ifndef CONCAT_CORPUS_H
#define CONCAT_CORPUS_H

#include "analysis.h"

/* Per-slice descriptor layout (NDESC dims). Indices are stable — the query and
 * nav-axis mapping depend on them. */
enum {
    /* means of per-frame descriptors */
    D_MFCC0_MEAN = 0,               /* .. D_MFCC0_MEAN+12 = MFCC 0..12 mean */
    D_CENTROID_MEAN = 13,
    D_FLATNESS_MEAN = 14,
    D_ZCR_MEAN      = 15,
    /* stds */
    D_MFCC0_STD  = 16,              /* .. +12 = MFCC 0..12 std */
    D_CENTROID_STD = 29,
    D_FLATNESS_STD = 30,
    D_ZCR_STD      = 31,
    /* scalar per-slice */
    D_LOUD_MEAN = 32,               /* RMS mean over frames */
    D_LOUD_PEAK = 33,               /* peak |sample| */
    D_PITCH     = 34,               /* median voiced pitch (Hz), 0 if unvoiced */
    D_PITCH_CONF= 35,               /* mean YIN confidence */
    NDESC = 36
};

typedef struct { int start; int len; } slice_range_t;

/* Onset-slice a mono float buffer. Spectral-flux ODF → median-subtract →
 * peak-pick + debounce; gaps clamped to [min_len,max_len] samples; slices with
 * RMS < sil_gate dropped. Writes up to max_slices ranges; returns the count. */
int concat_onset_slice(analysis_t *a, const float *buf, int len,
                       int min_len, int max_len, float sil_gate,
                       slice_range_t *out, int max_slices);

/* Fixed-grain slicing: contiguous grains of grain_len, 50% hop; silence-gated. */
int concat_fixed_slice(analysis_t *a, const float *buf, int len,
                       int grain_len, float sil_gate,
                       slice_range_t *out, int max_slices);

/* Compute a slice's NDESC descriptor vector (frames the slice, MultiStats). */
void concat_slice_descriptor(analysis_t *a, const float *slice, int len, float *out /*NDESC*/);

/* Fit per-dimension RobustScaling (median + IQR) over nslices × NDESC descriptors.
 * median[NDESC], iqr[NDESC] (IQR floored to a small epsilon to avoid /0). */
void concat_robust_fit(const float *desc, int nslices, float *median, float *iqr);

/* Apply RobustScaling: out[d] = (vec[d] - median[d]) / iqr[d]. */
void concat_robust_apply(const float *vec, const float *median, const float *iqr, float *out);

/* Fit per-dimension min/max over nslices × NDESC (for 0..1 nav-axis mapping). */
void concat_minmax_fit(const float *desc, int nslices, float *mn, float *mx);

#endif /* CONCAT_CORPUS_H */
