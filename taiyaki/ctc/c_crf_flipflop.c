#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <immintrin.h>

#include "c_crf_flipflop.h"
#include "sse_mathfun.h"
#include "avx_mathfun.h"

#define ALIGNMENT 8
#define LARGE_VAL 1e30f

static inline float logsumexpf(float x, float y, float a) {
    /* expf(-17.0f) is below float precision, and so zero */
    float delta = a * fabsf(x - y);
    return fmaxf(x, y) + ((delta < 17.0f) ? (log1pf(expf(-delta)) / a) : 0.0f);
}

static inline size_t nstate_to_nbase(size_t ntrans) {
    double nbase_d = sqrt(0.25 + (0.5 * ntrans)) - 0.5;
    assert(fmod(nbase_d, 1.0) == 0.0);
    return (size_t) round(nbase_d);
}

static inline float fmaxf_vec(const float *restrict x, size_t n) {
    float xmax = x[0];
    for (size_t i = 1; i < n; i++) {
        xmax = fmaxf(xmax, x[i]);
    }
    return xmax;
}

static inline float fmaxf_sse(const float *restrict x, size_t n) {
    const size_t n4 = n >> 2;
    const __m128 *restrict xV = (const __m128 * restrict) x;

    __m128 xmaxV = _mm_set1_ps(-HUGE_VALF);
    for (size_t i = 0; i < n4; i++) {
        xmaxV = _mm_max_ps(xmaxV, xV[i]);
    }
    float res[4] __attribute__ ((aligned(16)));
    _mm_store_ps(res, xmaxV);
    float xmax = fmaxf(fmaxf(res[0], res[1]), fmaxf(res[2], res[3]));

    for (size_t i = (n4 << 2); i < n; i++) {
        xmax = fmaxf(xmax, x[i]);
    }
    return xmax;
}

static inline float logsumexpf_vec(const float *restrict x, size_t n) {
    const float xmax = fmaxf_vec(x, n);
    float Z = expf(x[0] - xmax);
    for (size_t i = 1; i < n; i++) {
        Z += expf(x[i] - xmax);
    }
    return logf(Z);
}

static inline float logsumexpf_sse(const float *restrict x, size_t n) {
    const size_t n4 = n >> 2;
    const float xmax = fmaxf_sse(x, n);
    const __m128 xmaxV = _mm_set1_ps(xmax);
    const __m128 *restrict xV = (const __m128 * restrict) x;

    __m128 Z = _mm_setzero_ps();
    for (size_t i = 0; i < n4; i++) {
        Z += exp_ps(xV[i] - xmaxV);
    }

    //  Sum and finalise vectorised bit
    float Zout;
    Z = _mm_hadd_ps(Z, Z);
    Z = _mm_hadd_ps(Z, Z);
    _mm_store_ss(&Zout, Z);

    for (size_t i = (n4 << 2); i < n; i++) {
        //  Deal with remaining
        Zout += expf(x[i] - xmax);
    }

    return logf(Zout);
}

static inline void logaddexpf_sse(const float *restrict x, float *restrict y,
                                  float sharp, size_t n) {
    const size_t n4 = n >> 2;
    const __m128 *restrict xV = (const __m128 * restrict) x;
    __m128 *restrict yV = (__m128 * restrict) y;

    const __m128 sharpV = _mm_set1_ps(sharp);
    for (size_t i = 0; i < n4; i++) {
        const __m128 delta = xV[i] - yV[i];
        const __m128 abs_delta = _mm_max_ps(-delta, delta) * sharpV;
        const __m128 ones = _mm_set1_ps(1.0f);
        yV[i] =
            _mm_max_ps(xV[i],
                       yV[i]) + log_ps(ones + exp_ps(-abs_delta)) / sharp;
    }

    for (size_t i = (n4 << 2); i < n; i++) {
        //  Deal with remaining
        y[i] =
            fmaxf(x[i],
                  y[i]) + logf(1.0f +
                               expf(-sharp * fabsf(x[i] - y[i]))) / sharp;
    }
}

static inline float softmax_inplace_sse(float *restrict x, float sharp, size_t n) {
    const size_t n4 = n >> 2;
    __m128 *restrict xV = (__m128 * restrict) x;

    const __m128 sharpV = _mm_set1_ps(sharp);
    const float xmax = fmaxf_sse(x, n);
    const __m128 xmaxV = _mm_set1_ps(xmax);
    __m128 Z = _mm_setzero_ps();
    for (size_t i = 0; i < n4; i++) {
        xV[i] = exp_ps(sharpV * (xV[i] - xmaxV));
        Z += xV[i];
    }
    //  Sum and finalise vectorised bit
    float Zout;
    Z = _mm_hadd_ps(Z, Z);
    Z = _mm_hadd_ps(Z, Z);
    _mm_store_ss(&Zout, Z);

    for (size_t i = (n4 << 2); i < n; i++) {
        //  Deal with remaining
        x[i] = expf(sharp * (x[i] - xmax));
        Zout += x[i];
    }

    Z = _mm_set1_ps(Zout);
    for (size_t i = 0; i < n4; i++) {
        xV[i] /= Z;
    }
    for (size_t i = (n4 << 2); i < n; i++) {
        x[i] /= Zout;
    }

    return Zout;
}

static inline float fmaxf_avx(const float *restrict x, size_t n) {
    const size_t n4 = n >> 3;
    const __m256 *restrict xV = (const __m256 * restrict) x;

    __m256 xmaxV = _mm256_set1_ps(-HUGE_VALF);
    for (size_t i = 0; i < n4; i++) {
        xmaxV = _mm256_max_ps(xmaxV, xV[i]);
    }
    float res[8] __attribute__ ((aligned(32)));
    _mm256_store_ps(res, xmaxV);
    float xmax = fmax(fmaxf(fmaxf(res[0], res[1]), fmaxf(res[2], res[3])),
                      fmaxf(fmaxf(res[4], res[5]), fmaxf(res[6], res[7])));

    for (size_t i = (n4 << 3); i < n; i++) {
        xmax = fmaxf(xmax, x[i]);
    }
    return xmax;
}

static inline float logsumexpf_avx(const float *restrict x, size_t n) {
    const size_t n4 = n >> 3;
    const float xmax = fmaxf_avx(x, n);
    const __m256 xmaxV = _mm256_set1_ps(xmax);
    const __m256 *restrict xV = (const __m256 * restrict) x;

    __m256 Z = _mm256_setzero_ps();
    for (size_t i = 0; i < n4; i++) {
        Z += exp256_ps(xV[i] - xmaxV);
    }

    //  Sum and finalise vectorised bit
    float Zout;
    __m128 Zh = _mm256_castps256_ps128(Z) + _mm256_extractf128_ps(Z, 1);
    Zh = _mm_hadd_ps(Zh, Zh);
    Zh = _mm_hadd_ps(Zh, Zh);
    _mm_store_ss(&Zout, Zh);

    for (size_t i = (n4 << 3); i < n; i++) {
        //  Deal with remaining
        Zout += expf(x[i] - xmax);
    }

    return logf(Zout);
}

static inline void logaddexpf_avx(const float *restrict x, float *restrict y,
                                  float sharp, size_t n) {
    const size_t n4 = n >> 3;
    const __m256 *restrict xV = (const __m256 * restrict) x;
    __m256 *restrict yV = (__m256 * restrict) y;

    const __m256 sharpV = _mm256_set1_ps(sharp);
    const __m256 ones = _mm256_set1_ps(1.0f);
    for (size_t i = 0; i < n4; i++) {
        const __m256 delta = xV[i] - yV[i];
        const __m256 abs_delta = _mm256_max_ps(-delta, delta) * sharpV;
        yV[i] =
            _mm256_max_ps(xV[i],
                          yV[i]) + log256_ps(ones +
                                             exp256_ps(-abs_delta)) / sharpV;
    }

    for (size_t i = (n4 << 3); i < n; i++) {
        //  Deal with remaining
        y[i] =
            fmaxf(x[i],
                  y[i]) + logf(1.0f +
                               expf(-sharp * fabsf(x[i] - y[i]))) / sharp;
    }
}

static inline float softmax_inplace_avx(float *restrict x, float sharp, size_t n) {
    const size_t n4 = n >> 3;
    __m256 *restrict xV = (__m256 * restrict) x;

    const __m256 sharpV = _mm256_set1_ps(sharp);
    const float xmax = fmaxf_avx(x, n);
    const __m256 xmaxV = _mm256_set1_ps(xmax);
    __m256 Z = _mm256_setzero_ps();
    for (size_t i = 0; i < n4; i++) {
        xV[i] = exp256_ps(sharpV * (xV[i] - xmaxV));
        Z += xV[i];
    }

    //  Sum and finalise vectorised bit
    float Zout;
    __m128 Zh = _mm256_castps256_ps128(Z) + _mm256_extractf128_ps(Z, 1);
    Zh = _mm_hadd_ps(Zh, Zh);
    Zh = _mm_hadd_ps(Zh, Zh);
    _mm_store_ss(&Zout, Zh);

    for (size_t i = (n4 << 3); i < n; i++) {
        //  Deal with remaining
        x[i] = expf(sharp * (x[i] - xmax));
        Zout += x[i];
    }


    Z = _mm256_set1_ps(Zout);
    for (size_t i = 0; i < n4; i++) {
        xV[i] /= Z;
    }
    for (size_t i = (n4 << 3); i < n; i++) {
        x[i] /= Zout;
    }

    return Zout;
}


static inline size_t next_aligned(size_t i, size_t a) {
    const size_t mask = ~(a - 1);
    return (i + a - 1) & mask;
}


/**
 *  Training / ground truth functions
 **/

/*   Calculate forward probabilities for a flip-flop step

    Args:
       logprob (array [ntrans]):  Transition weights for block.
       fwdprev (array [nseqpos]):  Forward probs from previous block.
       moveidx (array [nseqpos - 1]):  Index for transitions moving to next
           position in sequence.
       stayidx (array [nseqpos]):  Index for transitions resulting in staying
           in position of sequence.
       nseqpos:  Length of sequence.
       fwdcurr (array [nseqpos]): OUT Forward probs for next block.
       sharpfact:  Sharpening factor, 1 = no sharpening.

    Returns:
       void:  Forward probabilities for next block writen to `fwdcurr`
*/
float crf_flipflop_forward_step(float const *restrict logprob,
                                float const *restrict fwdprev,
                                size_t const *restrict moveidx,
                                size_t const *restrict stayidx,
                                size_t nseqpos, float *restrict fwdcurr,
                                float *restrict fwdtmp, float sharpfact) {

    assert(nseqpos > 0);
    assert(NULL != logprob);
    assert(NULL != fwdprev);
    assert(NULL != moveidx);
    assert(NULL != stayidx);
    assert(NULL != fwdcurr);
    assert(NULL != fwdtmp);

    // loop through sequence
    for (size_t pos = 0; pos < nseqpos; pos++) {
        // Stay in current position
        fwdcurr[pos] = logprob[stayidx[pos]] + fwdprev[pos];
    }

    // loop through sequence positions we can step into
    fwdtmp[0] = -HUGE_VALF;
    for (size_t pos = 1; pos < nseqpos; pos++) {
        fwdtmp[pos] = fwdprev[pos - 1] + logprob[moveidx[pos - 1]];
        // in non-log space, we now have
        // fwdcurr[s] = fwdprev[s] * stayprob + fwdprev[s-1] * stepprob
    }
    logaddexpf_avx(fwdtmp, fwdcurr, sharpfact, nseqpos);

    const float factor = fmaxf_avx(fwdcurr, nseqpos);
    for (size_t pos = 0; pos < nseqpos; pos++) {
        fwdcurr[pos] -= factor;
    }
    return factor;
}

/*   Calculate forward probabilities for a flip-flop model

    Args:
       logprob (array [nblk x ntrans]):  Transition weights -- strided!
       nblk:  Number of blocks.
       ldp:  Stride for logprob matrix.
       moveidx (array [nseqpos - 1]):  Index for transitions moving to next
           position in sequence.
       stayidx (array [nseqpos]):  Index for transitions resulting in staying
           in position of sequence.
       nseqpos:  Length of sequence.
       sharpfact:  Sharpening factor, 1 = no sharpening.
       fwd (array [(nblk+1) x nseqpos]):  OUT Forward probabilities

    Returns:
       float:  Forwards score.  Forward probabilities for next block writen
    to `fwdcurr`.
*/
float crf_flipflop_forward(float const *restrict logprob, size_t nblk,
                           size_t ldp, size_t const *restrict moveidx,
                           size_t const *restrict stayidx, size_t nseqpos,
                           float sharpfact, float *restrict fwd) {

    assert(nseqpos > 0);
    assert(NULL != logprob);
    assert(NULL != moveidx);
    assert(NULL != stayidx);
    assert(NULL != fwd);

    const size_t nseqposAligned = next_aligned(nseqpos, ALIGNMENT);
    float *fwdtmp =
        aligned_alloc(4 * ALIGNMENT, nseqposAligned * sizeof(float));

    //  Point prior  -- must start in stay at beginning of sequence
    for (size_t pos = 0; pos < nseqpos; pos++) {
        fwd[pos] = -LARGE_VAL;
    }
    fwd[0] = 0.0;

    float score = 0.0f;
    for (size_t blk = 0; blk < nblk; blk++) {
        float const *fwdprev = fwd + blk * nseqposAligned;
        float *fwdcurr = fwd + (blk + 1) * nseqposAligned;
        float const *logprobcurr = logprob + blk * ldp;

        score +=
            crf_flipflop_forward_step(logprobcurr, fwdprev, moveidx, stayidx,
                                      nseqpos, fwdcurr, fwdtmp, sharpfact);
    }
    free(fwdtmp);

    // Final score is sum of final state + its stay
    score += fwd[nblk * nseqposAligned + nseqpos - 1];
    return score;
}

/*   Calculate backward probabilities for a flip-flop step

    Args:
       logprob (array [ntrans]):  Transition weights for block.
       bwdprev (array [nseqpos]):  Backward probs from previous block.
       moveidx (array [nseqpos - 1]):  Index for transitions moving to next
           position in sequence.
       stayidx (array [nseqpos]):  Index for transitions resulting in staying
           in position of sequence.
       nseqpos:  Length of sequence.
       bwdcurr (array [nseqpos]): OUT Backward probs for next block.
       sharpfact:  Sharpening factor, 1 = no sharpening.

    Returns:
       void:  Backward probabilities for next block writen to `bwdcurr`
*/
float crf_flipflop_backward_step(float const *restrict logprob,
                                 float const *restrict bwdprev,
                                 size_t const *restrict moveidx,
                                 size_t const *restrict stayidx,
                                 size_t nseqpos, float *restrict bwdcurr,
                                 float *restrict bwdtmp, float sharpfact) {

    assert(nseqpos > 0);
    assert(NULL != logprob);
    assert(NULL != bwdprev);
    assert(NULL != moveidx);
    assert(NULL != stayidx);
    assert(NULL != bwdcurr);
    assert(NULL != bwdtmp);

    for (size_t pos = 0; pos < nseqpos; pos++) {
        // Stay in current position
        bwdcurr[pos] = logprob[stayidx[pos]] + bwdprev[pos];
    }
    bwdtmp[nseqpos - 1] = -HUGE_VALF;
    for (size_t pos = 1; pos < nseqpos; pos++) {
        // Move to new position
        bwdtmp[pos - 1] = bwdprev[pos] + logprob[moveidx[pos - 1]];
    }

    logaddexpf_avx(bwdtmp, bwdcurr, sharpfact, nseqpos);

    const float factor = fmaxf_avx(bwdcurr, nseqpos);
    for (size_t pos = 0; pos < nseqpos; pos++) {
        bwdcurr[pos] -= factor;
    }
    return factor;
}

/*   Calculate backward probabilities for a flip-flop model

    Args:
       logprob (array [ntrans]):  Transition weights for block.
       nblk:  Number of blocks.
       ldp:  Stride for logprob matrix.
       moveidx (array [nseqpos - 1]):  Index for transitions moving to next
           position in sequence.
       stayidx (array [nseqpos]):  Index for transitions resulting in staying
           in position of sequence.
       nseqpos:  Length of sequence.
       sharpfact:  Sharpening factor, 1 = no sharpening.
       bwd (array [(nblk+1) x nseqpos]):  OUT Backward probabilities

    Returns:
       float:  Backwards score.  Backward probabilities for next block written
       to `bwdcurr`.
*/
float crf_flipflop_backward(float const *restrict logprob, size_t nblk,
                            size_t ldp, size_t const *restrict moveidx,
                            size_t const *restrict stayidx, size_t nseqpos,
                            float sharpfact, float *restrict bwd) {
    assert(nseqpos > 0);
    assert(NULL != logprob);
    assert(NULL != moveidx);
    assert(NULL != stayidx);
    assert(NULL != bwd);

    const size_t nseqposAligned = next_aligned(nseqpos, ALIGNMENT);
    float *bwdtmp =
        aligned_alloc(4 * ALIGNMENT, nseqposAligned * sizeof(float));

    //  Point prior -- must have ended in either final stay or state
    for (size_t pos = 0; pos < nseqpos; pos++) {
        bwd[nblk * nseqposAligned + pos] = -LARGE_VAL;
    }
    // Final stay
    bwd[nblk * nseqposAligned + nseqpos - 1] = 0.0;

    float score = 0.0f;
    for (size_t blk = nblk; blk > 0; blk--) {
        float const *bwdprev = bwd + blk * nseqposAligned;
        float *bwdcurr = bwd + (blk - 1) * nseqposAligned;
        float const *logprobcurr = logprob + (blk - 1) * ldp;

        score +=
            crf_flipflop_backward_step(logprobcurr, bwdprev, moveidx, stayidx,
                                       nseqpos, bwdcurr, bwdtmp, sharpfact);
    }

    free(bwdtmp);
    return bwd[0] + score;
}

/*   Calculate forward probabilities for a flip-flop model for each element of
 batch

    Args:
       logprob (array [nblk x nbatch x ntrans]):  Transition weights
       ntrans:  Number of transitions.
       nblk:  Number of blocks.
       nbatch:  Size of batch.
       moveidxs (array [sum(seqlen) - nbatch]):  Index for transitions moving
     to next position in sequence.
       stayidxs (array [sum(seqlen)]):  Index for transitions resulting in
     staying in position of sequence.
       seqlen (array [nbatch]):  Length of sequences.
       sharpfact:  Sharpening factor, 1 = no sharpening.
       score (array [nbatch]):  OUT Forward scores

    Returns:
       void:  Forward scores written to `score`
*/
void crf_flipflop_cost(float const *logprob, size_t ntrans, size_t nblk,
                       size_t nbatch, size_t const *moveidxs,
                       size_t const *stayidxs, int32_t const *seqlen,
                       float sharpfact, float *score) {

    const size_t ldp = nbatch * ntrans;
    size_t seqidx[nbatch];
    seqidx[0] = 0;
    for (size_t idx = 1; idx < nbatch; idx++) {
        seqidx[idx] = seqidx[idx - 1] + seqlen[idx - 1];
    }

#pragma omp parallel for
    for (size_t batch = 0; batch < nbatch; batch++) {
        if (0 == seqlen[batch]) {
            score[batch] = 0.0;
            continue;
        }

        const size_t batch_offset = batch * ntrans;
        const size_t nseqposAligned = next_aligned(seqlen[batch], ALIGNMENT);
        float *fwd =
            aligned_alloc(4 * ALIGNMENT,
                          (1 + nblk) * nseqposAligned * sizeof(float));
        if (NULL == fwd) {
            free(fwd);
            score[batch] = NAN;
            continue;
        }
        score[batch] = crf_flipflop_forward(logprob + batch_offset, nblk, ldp,
                                            //  1 less move per sequence than positions
                                            moveidxs + seqidx[batch] - batch,
                                            stayidxs + seqidx[batch],
                                            seqlen[batch], sharpfact, fwd);
        free(fwd);
    }
}

/*    Forward calculation of scores. Wrapper for crf_flipflop_cost()
*/
void crf_flipflop_scores_fwd(float const *logprob, size_t ntrans, size_t nblk,
                             size_t nbatch, size_t const *moveidxs,
                             size_t const *stayidxs, int32_t const *seqlen,
                             float sharpfact, float *score) {
    crf_flipflop_cost(logprob, ntrans, nblk, nbatch, moveidxs, stayidxs,
                      seqlen, sharpfact, score);
}

/*   Calculate backward probabilities for a flip-flop model for each element
 of batch

    Args:
       logprob (array [nblk x nbatch x ntrans]):  Transition weights
       ntrans:  Number of transitions.
       nblk:  Number of blocks.
       nbatch:  Size of batch.
       moveidxs (array [sum(seqlen) - nbatch]):  Index for transitions moving
           to next position in sequence.
       stayidxs (array [sum(seqlen)]):  Index for transitions resulting in
           staying in position of sequence.
       seqlen (array [nbatch]):  Length of sequences.
       sharpfact:  Sharpening factor, 1 = no sharpening.
       score (array [nbatch]):  OUT Backward scores

    Returns:
       void:  Backward scores written to `score`
*/
void crf_flipflop_scores_bwd(float const *logprob, size_t ntrans, size_t nblk,
                             size_t nbatch, size_t const *moveidxs,
                             size_t const *stayidxs, int32_t const *seqlen,
                             float sharpfact, float *score) {
    const size_t ldp = nbatch * ntrans;
    size_t seqidx[nbatch];
    seqidx[0] = 0;
    for (size_t idx = 1; idx < nbatch; idx++) {
        seqidx[idx] = seqidx[idx - 1] + seqlen[idx - 1];
    }

#pragma omp parallel for
    for (size_t batch = 0; batch < nbatch; batch++) {
        if (0 == seqlen[batch]) {
            score[batch] = 0.0;
            continue;
        }
        const size_t offset = batch * ntrans;
        const size_t nseqposAligned = next_aligned(seqlen[batch], ALIGNMENT);
        float *bwd =
            aligned_alloc(4 * ALIGNMENT,
                          (1 + nblk) * nseqposAligned * sizeof(float));
        if (NULL == bwd) {
            free(bwd);
            score[batch] = NAN;
            continue;
        }
        score[batch] = crf_flipflop_backward(logprob + offset, nblk, ldp,
                                             //  1 less move per sequence than sequence
                                             moveidxs + seqidx[batch] - batch,
                                             stayidxs + seqidx[batch],
                                             seqlen[batch], sharpfact, bwd);
        free(bwd);
    }
}

/*    Calculate flipflop gradients for a single block

    Args:
       fwdcurr (array [nseqpos]):  Forward probs for current block.
       bwdnext (array [nseqpos]):  Backward probs from next block.
       logprob (array [ntrans]):  Transition weights for block.
       moveidx (array [nseqpos - 1]):  Index for transitions moving to next
           position in sequence.
       stayidx (array [nseqpos]):  Index for transitions resulting in staying
           in position of sequence.
       nseqpos:  Length of sequence.
       grad (array[ntrans]):  OUT  Gradients for block.
       ntrans:  Number of possible transitions.
       fact:  Scaling factor (log partition function).
       sharpfact:  Sharpening factor, 1 = no sharpening.

    Returns:
       void:  Gradients written to `grad`.
*/
void crf_flipflop_grad_step(float const *restrict fwdcurr,
                            float const *restrict bwdnext,
                            float const *restrict logprob,
                            size_t const *restrict moveidx,
                            size_t const *restrict stayidx, size_t nseqpos,
                            float *restrict grad, float *restrict gradtmp,
                            size_t ntrans, float fact, float sharpfact) {

    assert(NULL != fwdcurr);
    assert(NULL != bwdnext);
    assert(NULL != logprob);
    assert(NULL != moveidx);
    assert(NULL != stayidx);
    assert(NULL != grad);
    assert(NULL != gradtmp);

    // Make sure gradient calc is zero'd
    memset(grad, 0, ntrans * sizeof(float));

    for (size_t pos = 0; pos < nseqpos; pos++) {
        const size_t idx = stayidx[pos];
        gradtmp[pos] = fwdcurr[pos] + bwdnext[pos] + logprob[idx];
    }
    for (size_t pos = 0; pos < nseqpos - 1; pos++) {
        const size_t idx = moveidx[pos];
        gradtmp[pos + nseqpos] = fwdcurr[pos] + bwdnext[pos + 1] + logprob[idx];
    }

    const size_t N = nseqpos + nseqpos - 1;
    softmax_inplace_avx(gradtmp, sharpfact, N);

    for (size_t pos = 0; pos < nseqpos; pos++) {
        // Stay state
        const size_t idx = stayidx[pos];
        grad[idx] += gradtmp[pos];
    }
    for (size_t pos = 0; pos < nseqpos - 1; pos++) {
        // Move state
        const size_t idx = moveidx[pos];
        grad[idx] += gradtmp[pos + nseqpos];
    }
}

/*   Calculate gradients for a flip-flop model for each element of batch

    Args:
       logprob (array [nblk x nbatch x ntrans]):  Transition weights --
           strided!
       ntrans:  Number of transitions.
       nblk:  Number of blocks.
       nbatch:  Size of batch.
       moveidxs (array [sum(seqlen) - nbatch]):  Index for transitions moving
           to next position in sequence.
       stayidxs (array [sum(seqlen)]):  Index for transitions resulting in
           staying in position of sequence.
       seqlen (array [nbatch]):  Length of sequences.
       sharpfact:  Sharpening factor, 1 = no sharpening.
       score (array [nbatch]):  OUT scores
       grad (array [nblk x nbatch x ntrans]):  OUT Gradients

    Returns:
       void:  Backward scores written to `score`
*/
void crf_flipflop_grad(float const *logprob, size_t ntrans, size_t nblk,
                       size_t nbatch, size_t const *moveidxs,
                       size_t const *stayidxs, int32_t const *seqlen,
                       float sharpfact, float *score, float *grad) {

    assert(NULL != logprob);
    assert(NULL != moveidxs);
    assert(NULL != stayidxs);
    assert(NULL != seqlen);
    assert(NULL != score);
    assert(NULL != grad);

    const size_t ldp = nbatch * ntrans;
    size_t seqidx[nbatch];
    seqidx[0] = 0;
    for (size_t idx = 1; idx < nbatch; idx++) {
        seqidx[idx] = seqidx[idx - 1] + seqlen[idx - 1];
    }

#pragma omp parallel for
    // Loop over batch elements
    for (size_t batch = 0; batch < nbatch; batch++) {
        const size_t batch_offset = batch * ntrans;
        // No sequence for this batch element
        if (0 == seqlen[batch]) {
            for (size_t blk = 0; blk < nblk; blk++) {
                memset(grad + batch_offset + blk * ldp, 0,
                       ntrans * sizeof(float));
            }
            continue;
        }
        const size_t nseqpos = seqlen[batch];
        const size_t nseqposAligned = next_aligned(seqlen[batch], ALIGNMENT);
        // space for a forward matrix
        float *fwd =
            aligned_alloc(4 * ALIGNMENT,
                          (nblk + 1) * nseqposAligned * sizeof(float));
        // ...and a backward matrix
        float *bwd =
            aligned_alloc(4 * ALIGNMENT,
                          (nblk + 1) * nseqposAligned * sizeof(float));
        if (NULL == fwd || NULL == bwd) {
            free(fwd);
            free(bwd);
            // TODO set grad values to NAN in order to propogate memory error
            continue;
        }
        size_t const *moveidx = moveidxs + seqidx[batch] - batch;
        size_t const *stayidx = stayidxs + seqidx[batch];
        // Calculate forward score and forward matrix for one batch element
        score[batch] =
            crf_flipflop_forward(logprob + batch_offset, nblk, ldp, moveidx,
                                 stayidx, nseqpos, sharpfact, fwd);
        // TODO compute backwards while computing gradient to reduce memory
        // footprint
        // backward matrix for one batch element
        score[batch] += crf_flipflop_backward(logprob + batch_offset, nblk, ldp,
                                              moveidx, stayidx, nseqpos,
                                              sharpfact, bwd);
        score[batch] *= 0.5;

        // Normalised transition matrix
        // loop over blocks
        float *gradtmp =
            aligned_alloc(4 * ALIGNMENT,
                          (nseqposAligned + nseqposAligned) * sizeof(float));
        for (size_t blk = 0; blk < nblk; blk++) {
            // forward matrix column for this batch element and block
            float const *fwdcurr = fwd + blk * nseqposAligned;
            float const *bwdcurr = bwd + blk * nseqposAligned;
            float const *bwdnext = bwd + blk * nseqposAligned + nseqposAligned;
            float const *logprobcurr = logprob + batch_offset + blk * ldp;
            // pointer to part of grad matrix referring to this batch element
            // and this block
            float *gradcurr = grad + batch_offset + blk * ldp;

            // put gradients into gradcurr
            crf_flipflop_grad_step(fwdcurr, bwdnext, logprobcurr, moveidx,
                                   stayidx, nseqpos, gradcurr, gradtmp, ntrans,
                                   score[batch], sharpfact);
        }

        free(gradtmp);
        free(bwd);
        free(fwd);
    }
}

#ifdef CRF_TWOSTATE_TEST

const int32_t test_seq1[12] = { 0, 1, 5, 1, 3, 2,
    0, 1, 5, 1, 3, 2
};

const size_t test_move1[12] = { 8, 33, 13, 25, 19,
    8, 33, 13, 25, 19
};

const size_t test_stay1[12] = { 0, 9, 37, 9, 27, 18,
    0, 9, 37, 9, 27, 18
};

const int32_t test_seqlen1[2] = { 6, 6 };

float test_logprob1[560] = {
    // t = 0, blk = 0 -- stay in 0
    0.7137395145, 0.0058640570, 0.0043273252, 0.0057024065, 0.0001304555,
    0.0167860687, 0.0014591201, 0.0039324691,
    0.0117071924, 0.0045297625, 0.0105104226, 0.0018303745, 0.0004133878,
    0.0121020079, 0.0179132788, 0.0008446391,
    0.0003954364, 0.0046109826, 0.0061280611, 0.0037487558, 0.0002867797,
    0.0021094619, 0.0090478168, 0.0088021810,
    0.0166425156, 0.0008985700, 0.0030807985, 0.0150129722, 0.0033072104,
    0.0225965258, 0.0017120223, 0.0080003635,
    0.0086164755, 0.0085638228, 0.0090326148, 0.0184277679, 0.0128914220,
    0.0024000880, 0.0143853339, 0.0075095396,
    // t = 0, blk = 1
    0.7137395145, 0.0058640570, 0.0043273252, 0.0057024065, 0.0001304555,
    0.0167860687, 0.0014591201, 0.0039324691,
    0.0117071924, 0.0045297625, 0.0105104226, 0.0018303745, 0.0004133878,
    0.0121020079, 0.0179132788, 0.0008446391,
    0.0003954364, 0.0046109826, 0.0061280611, 0.0037487558, 0.0002867797,
    0.0021094619, 0.0090478168, 0.0088021810,
    0.0166425156, 0.0008985700, 0.0030807985, 0.0150129722, 0.0033072104,
    0.0225965258, 0.0017120223, 0.0080003635,
    0.0086164755, 0.0085638228, 0.0090326148, 0.0184277679, 0.0128914220,
    0.0024000880, 0.0143853339, 0.0075095396,

    // t = 1, blk = 0 -- move 0 to 1
    0.0138651518, 0.0068715546, 0.0137762669, 0.0142378858, 0.0038887475,
    0.0002837213, 0.0009213002, 0.0046096374,
    0.7005158726, 0.0041189393, 0.0057012358, 0.0196555714, 0.0034917922,
    0.0031160895, 0.0027309383, 0.0068903076,
    0.0016565445, 0.0013069584, 0.0067694923, 0.0071836470, 0.0012639324,
    0.0110877851, 0.0064367276, 0.0085079412,
    0.0003521574, 0.0035635810, 0.0043749238, 0.0027222466, 0.0139259729,
    0.0152291942, 0.0044505049, 0.0039157630,
    0.0096219943, 0.0208794052, 0.0031593320, 0.0516253381, 0.0051720879,
    0.0038762056, 0.0067444477, 0.0014988048,
    // t = 1, blk = 1
    0.0138651518, 0.0068715546, 0.0137762669, 0.0142378858, 0.0038887475,
    0.0002837213, 0.0009213002, 0.0046096374,
    0.7005158726, 0.0041189393, 0.0057012358, 0.0196555714, 0.0034917922,
    0.0031160895, 0.0027309383, 0.0068903076,
    0.0016565445, 0.0013069584, 0.0067694923, 0.0071836470, 0.0012639324,
    0.0110877851, 0.0064367276, 0.0085079412,
    0.0003521574, 0.0035635810, 0.0043749238, 0.0027222466, 0.0139259729,
    0.0152291942, 0.0044505049, 0.0039157630,
    0.0096219943, 0.0208794052, 0.0031593320, 0.0516253381, 0.0051720879,
    0.0038762056, 0.0067444477, 0.0014988048,

    // t = 2, blk = 0 -- move 1 to 5
    0.0104973116, 0.0278749046, 0.0016333734, 0.0132478834, 0.0108985734,
    0.0326813004, 0.0104401808, 0.0281931252,
    0.0002602418, 0.0004849826, 0.0069461090, 0.0337142774, 0.0066522165,
    0.0002687968, 0.0081917502, 0.0014596191,
    0.0033038509, 0.0071742025, 0.0079209436, 0.0027446117, 0.0001922884,
    0.0002173728, 0.0022822792, 0.0063767010,
    0.0062269709, 0.0008360773, 0.0009815072, 0.0138239322, 0.0006819603,
    0.0004184386, 0.0005169712, 0.0038701156,
    0.0018582183, 0.7184016070, 0.0038719050, 0.0057834926, 0.0016248741,
    0.0121355831, 0.0023164603, 0.0029949899,
    // t = 2, blk = 1
    0.0104973116, 0.0278749046, 0.0016333734, 0.0132478834, 0.0108985734,
    0.0326813004, 0.0104401808, 0.0281931252,
    0.0002602418, 0.0004849826, 0.0069461090, 0.0337142774, 0.0066522165,
    0.0002687968, 0.0081917502, 0.0014596191,
    0.0033038509, 0.0071742025, 0.0079209436, 0.0027446117, 0.0001922884,
    0.0002173728, 0.0022822792, 0.0063767010,
    0.0062269709, 0.0008360773, 0.0009815072, 0.0138239322, 0.0006819603,
    0.0004184386, 0.0005169712, 0.0038701156,
    0.0018582183, 0.7184016070, 0.0038719050, 0.0057834926, 0.0016248741,
    0.0121355831, 0.0023164603, 0.0029949899,

    // t = 3, blk = 0 -- stay in 5
    0.0132238486, 0.0067462421, 0.0065735995, 0.0002313058, 0.0350482900,
    0.0038167453, 0.0013436872, 0.0047910351,
    0.0005511208, 0.0152455357, 0.0002505248, 0.0009566527, 0.0016608534,
    0.0036526310, 0.0038930839, 0.0102019269,
    0.0040538124, 0.0121608248, 0.0026858640, 0.0024698387, 0.0077258147,
    0.0063036375, 0.0015254714, 0.0015248249,
    0.0008483379, 0.0194108435, 0.0065140833, 0.0189690442, 0.0005446999,
    0.0072716624, 0.0002782992, 0.0124768655,
    0.0239038132, 0.0108786276, 0.0208670656, 0.0076679875, 0.0086667116,
    0.7072362974, 0.0038886950, 0.0039397951,
    // t = 3, blk = 1
    0.0132238486, 0.0067462421, 0.0065735995, 0.0002313058, 0.0350482900,
    0.0038167453, 0.0013436872, 0.0047910351,
    0.0005511208, 0.0152455357, 0.0002505248, 0.0009566527, 0.0016608534,
    0.0036526310, 0.0038930839, 0.0102019269,
    0.0040538124, 0.0121608248, 0.0026858640, 0.0024698387, 0.0077258147,
    0.0063036375, 0.0015254714, 0.0015248249,
    0.0008483379, 0.0194108435, 0.0065140833, 0.0189690442, 0.0005446999,
    0.0072716624, 0.0002782992, 0.0124768655,
    0.0239038132, 0.0108786276, 0.0208670656, 0.0076679875, 0.0086667116,
    0.7072362974, 0.0038886950, 0.0039397951,

    // t = 4, blk = 0 -- move 5 to 1
    0.0162499295, 0.0042696969, 0.0190051755, 0.0162959320, 0.0038385851,
    0.0010900080, 0.0051636429, 0.0088802400,
    0.0035193397, 0.0100004109, 0.0182444400, 0.0002015949, 0.0051056114,
    0.7237303612, 0.0135142243, 0.0065390854,
    0.0029951279, 0.0029123437, 0.0010848643, 0.0320041842, 0.0029855054,
    0.0001557548, 0.0043323211, 0.0161734933,
    0.0051668898, 0.0007899601, 0.0024293827, 0.0107437912, 0.0005963283,
    0.0004204642, 0.0008271684, 0.0036831630,
    0.0058302092, 0.0044612666, 0.0090699795, 0.0135366090, 0.0087714458,
    0.0033968323, 0.0002088134, 0.0117758241,
    // t = 4, blk = 1
    0.0162499295, 0.0042696969, 0.0190051755, 0.0162959320, 0.0038385851,
    0.0010900080, 0.0051636429, 0.0088802400,
    0.0035193397, 0.0100004109, 0.0182444400, 0.0002015949, 0.0051056114,
    0.7237303612, 0.0135142243, 0.0065390854,
    0.0029951279, 0.0029123437, 0.0010848643, 0.0320041842, 0.0029855054,
    0.0001557548, 0.0043323211, 0.0161734933,
    0.0051668898, 0.0007899601, 0.0024293827, 0.0107437912, 0.0005963283,
    0.0004204642, 0.0008271684, 0.0036831630,
    0.0058302092, 0.0044612666, 0.0090699795, 0.0135366090, 0.0087714458,
    0.0033968323, 0.0002088134, 0.0117758241,

    // t = 5, blk = 0 -- move 1 to 3
    0.0054995373, 0.0003135968, 0.0036685129, 0.0239510419, 0.0039243790,
    0.0019827996, 0.0129521071, 0.0066243852,
    0.0072536818, 0.0159209645, 0.0116239255, 0.0211135167, 0.0071678950,
    0.0168522449, 0.0034948831, 0.0148879133,
    0.0084620257, 0.0075577618, 0.0042788046, 0.0007793942, 0.0038023124,
    0.0116145280, 0.0025982395, 0.0022352670,
    0.0019744321, 0.7117781744, 0.0044554214, 0.0010030397, 0.0047838417,
    0.0005540779, 0.0085588124, 0.0001078087,
    0.0019562465, 0.0097635189, 0.0012854310, 0.0076597643, 0.0032004197,
    0.0354927128, 0.0017610103, 0.0071055704,
    // t = 5, blk = 1
    0.0054995373, 0.0003135968, 0.0036685129, 0.0239510419, 0.0039243790,
    0.0019827996, 0.0129521071, 0.0066243852,
    0.0072536818, 0.0159209645, 0.0116239255, 0.0211135167, 0.0071678950,
    0.0168522449, 0.0034948831, 0.0148879133,
    0.0084620257, 0.0075577618, 0.0042788046, 0.0007793942, 0.0038023124,
    0.0116145280, 0.0025982395, 0.0022352670,
    0.0019744321, 0.7117781744, 0.0044554214, 0.0010030397, 0.0047838417,
    0.0005540779, 0.0085588124, 0.0001078087,
    0.0019562465, 0.0097635189, 0.0012854310, 0.0076597643, 0.0032004197,
    0.0354927128, 0.0017610103, 0.0071055704,

    // t = 6, blk = 0 -- move 3 to 2
    0.0027753489, 0.0042800652, 0.0131082339, 0.0027542745, 0.0073969560,
    0.0022332778, 0.0063905429, 0.0225312653,
    0.0083716146, 0.0018647020, 0.0080511935, 0.0062377027, 0.0096483698,
    0.0050934491, 0.0002518356, 0.0089501860,
    0.0019424988, 0.0028867039, 0.0362414220, 0.7084635261, 0.0012042079,
    0.0016243873, 0.0089677837, 0.0001407093,
    0.0007788545, 0.0061531496, 0.0116723082, 0.0160689361, 0.0045947877,
    0.0025051798, 0.0016243552, 0.0025087153,
    0.0037103848, 0.0021407879, 0.0141961964, 0.0206362499, 0.0234809816,
    0.0151728742, 0.0018537195, 0.0014922626,
    // t = 6, blk = 1
    0.0027753489, 0.0042800652, 0.0131082339, 0.0027542745, 0.0073969560,
    0.0022332778, 0.0063905429, 0.0225312653,
    0.0083716146, 0.0018647020, 0.0080511935, 0.0062377027, 0.0096483698,
    0.0050934491, 0.0002518356, 0.0089501860,
    0.0019424988, 0.0028867039, 0.0362414220, 0.7084635261, 0.0012042079,
    0.0016243873, 0.0089677837, 0.0001407093,
    0.0007788545, 0.0061531496, 0.0116723082, 0.0160689361, 0.0045947877,
    0.0025051798, 0.0016243552, 0.0025087153,
    0.0037103848, 0.0021407879, 0.0141961964, 0.0206362499, 0.0234809816,
    0.0151728742, 0.0018537195, 0.0014922626
};

#include <stdio.h>

int main(int argc, char *argv[]) {

    const size_t nblk = 7;
    const size_t nstate = 40;
    const size_t nbatch = 2;
    float score[2] = { 0.0f };
    float score2[2] = { 0.0f };
    const float DELTA = 1e-2f;
    const float sharpfact = (argc > 1) ? atof(argv[1]) : 1.0f;
    const size_t msize = nblk * nstate * nbatch;

    float T[16];
    for (size_t t = 0; t < 16; t++) {
        fprintf(stdout, "** t = %zu\n", t);
        for (size_t i = 0; i < 16; i++) {
            T[i] = (float) i;
        }
        T[0] = 17.0;
        fprintf(stdout, "SSE fwd = %f\n", softmax_inplace_sse(T, sharpfact, 16 - t));
        for (size_t i = 0; i < 16; i++) {
            T[i] = (float) i;
        }
        T[0] = 17.0;
        fprintf(stdout, "AVX fwd = %f\n", softmax_inplace_avx(T, sharpfact, 16 - t));
        for (size_t i = 0; i < 16; i++) {
            T[i] = (float) (16 - i);
        }
        T[0] = 17.0;
        fprintf(stdout, "SSE bwd = %f\n", softmax_inplace_sse(T, sharpfact, 16 - t));
        for (size_t i = 0; i < 16; i++) {
            T[i] = (float) (16 - i);
        }
        T[0] = 17.0;
        fprintf(stdout, "AVX bwd = %f\n", softmax_inplace_avx(T, sharpfact, 16 - t));
    }


    for (size_t i = 0; i < msize; i++) {
        test_logprob1[i] = logf(test_logprob1[i]);
    }

    //
    //    F / B calculations
    //
    crf_flipflop_scores_fwd(test_logprob1, nstate, nblk, nbatch, test_move1,
                            test_stay1, test_seqlen1, sharpfact, score);
    printf("Forwards scores: %f %f\n", score[0], score[1]);

    crf_flipflop_scores_bwd(test_logprob1, nstate, nblk, nbatch, test_move1,
                            test_stay1, test_seqlen1, sharpfact, score);
    printf("Backwards scores: %f %f\n", score[0], score[1]);

    float *grad = calloc(msize, sizeof(float));
    crf_flipflop_grad(test_logprob1, nstate, nblk, nbatch, test_move1,
                      test_stay1, test_seqlen1, sharpfact, score2, grad);
    float maxdelta = 0.0;
    for (size_t blk = 0; blk < nblk; blk++) {
        const size_t offset = blk * nbatch * nstate;
        for (size_t st = 0; st < nstate; st++) {
            maxdelta = fmaxf(maxdelta, fabsf(grad[offset + st] -
                                             grad[offset + nstate + st]));
        }
    }
    printf("Max grad delta = %f\n", maxdelta);

    printf("Derviatives:\n");
    float fscore[2] = { 0.0f };
    for (size_t blk = 0; blk < nblk; blk++) {
        printf("  Block %zu\n", blk);
        const size_t offset = blk * nbatch * nstate;
        for (size_t st = 0; st < nstate; st++) {
            // Positive difference
            const float orig = test_logprob1[offset + st];
            test_logprob1[offset + st] = orig + DELTA;
            crf_flipflop_scores_fwd(test_logprob1, nstate, nblk, nbatch,
                                    test_move1, test_stay1, test_seqlen1,
                                    sharpfact, score);
            fscore[0] = score[0];
            fscore[1] = score[1];
            // Negative difference
            test_logprob1[offset + st] = orig - DELTA;
            crf_flipflop_scores_fwd(test_logprob1, nstate, nblk, nbatch,
                                    test_move1, test_stay1, test_seqlen1,
                                    sharpfact, score);
            fscore[0] = (fscore[0] - score[0]) / (2.0f * DELTA);
            fscore[1] = (fscore[1] - score[1]) / (2.0f * DELTA);
            // Report and reset
            test_logprob1[offset + st] = orig;
            printf("    %f d=%f r=%f [%f %f]\n",
                   grad[offset + st], fabsf(grad[offset + st] - fscore[0]),
                   grad[offset + st] / fscore[0], fscore[0], fscore[1]);
        }
    }

}
#endif                          /* CRF_TWOSTATE_TEST */

#ifdef SPEED_TEST
#include <stdio.h>
#include <time.h>


float runif(void) {
    return random() / (RAND_MAX + 1.0);
}


size_t state_of_move(size_t from, size_t to, size_t nbase) {
    return from + ((to < nbase) ? to : nbase) * (nbase + nbase);
}


size_t state_of_stay(size_t state, size_t nbase) {
    return state_of_move(state, state, nbase);
}


int main(int argc, char *argv[]) {
    const size_t nbase = 4;
    const size_t ntrans = 40;
    const float sharpfact = 1.0f;

    if (argc != 5) {
        fputs("Usage: nbatch nblock ntimes seed\n", stderr);
        exit(EXIT_FAILURE);
    }
    const size_t nbatch = atoi(argv[1]);
    const size_t nblock = atoi(argv[2]);
    const size_t ntimes = atoi(argv[3]);
    srandom(atoi(argv[4]));

    fputs("* Generating logprob\n", stdout);
    const size_t logprob_nelt = nblock * nbatch * ntrans;
    float *logprob = calloc(logprob_nelt, sizeof(float));
    for (size_t i = 0; i < logprob_nelt; i++) {
        logprob[i] = 10.0 * runif() - 5.0;
    }

    fputs("* Generating sequences\n", stdout);
    int32_t *seqlen = calloc(nbatch, sizeof(int32_t));
    size_t tot_seqlen = 0;
    for (size_t i = 0; i < nbatch; i++) {
        seqlen[i] = nblock * (1 + (i - 0.5f * nbatch) / (5.0f * nbatch)) / 2;
        tot_seqlen += seqlen[i];
        fprintf(stderr, "Seq %zu is %d\n", i, seqlen[i]);
    }

    int32_t *seqs = calloc(tot_seqlen, sizeof(int32_t));
    size_t *moves = calloc(tot_seqlen - nbatch, sizeof(size_t));
    size_t *stays = calloc(tot_seqlen, sizeof(size_t));
    for (size_t i = 0, j = 0; i < nbatch; i++) {
        seqs[j] = random() % nbase;
        stays[j] = state_of_stay(seqs[j], nbase);
        j += 1;
        for (size_t k = 1; k < seqlen[i]; j++, k++) {
            seqs[j] = random() % nbase;
            if (seqs[j] == seqs[j - 1]) {
                seqs[j] += nbase;
            }
            stays[j] = state_of_stay(seqs[j], nbase);
            moves[j - i - 1] = state_of_move(seqs[j - 1], seqs[j], nbase);
        }
    }


    float *score = calloc(nbatch, sizeof(float));
    {
        fprintf(stdout, "Timing %zu reps of crf_flipflop_scores_fwd\n", ntimes);
        struct timespec start_time;
        clock_gettime(CLOCK_MONOTONIC, &start_time);
        for (size_t i = 0; i < ntimes; i++) {
            fprintf(stdout, "Round %zu:\n", i);
            crf_flipflop_scores_fwd(logprob, ntrans, nblock, nbatch,
                                    moves, stays, seqlen, sharpfact, score);
            for (size_t j = 0; j < nbatch; j++) {
                fprintf(stdout, "%f\n", score[j]);
            }
        }
        struct timespec end_time;
        clock_gettime(CLOCK_MONOTONIC, &end_time);
        int start_time_ms =
            (start_time.tv_sec * 1000 + start_time.tv_nsec / 1000000);
        int end_time_ms = (end_time.tv_sec * 1000 + end_time.tv_nsec / 1000000);
        int delta_time_ms = end_time_ms - start_time_ms;
        fprintf(stdout, "%ds %dms\n", delta_time_ms / 1000,
                delta_time_ms % 1000);
    }

    {
        fprintf(stdout, "Timing %zu reps of crf_flipflop_scores_bwd\n", ntimes);
        struct timespec start_time;
        clock_gettime(CLOCK_MONOTONIC, &start_time);
        for (size_t i = 0; i < ntimes; i++) {
            fprintf(stdout, "Round %zu:\n", i);
            crf_flipflop_scores_bwd(logprob, ntrans, nblock, nbatch,
                                    moves, stays, seqlen, sharpfact, score);
            for (size_t j = 0; j < nbatch; j++) {
                fprintf(stdout, "%f\n", score[j]);
            }
        }
        struct timespec end_time;
        clock_gettime(CLOCK_MONOTONIC, &end_time);
        int start_time_ms =
            (start_time.tv_sec * 1000 + start_time.tv_nsec / 1000000);
        int end_time_ms = (end_time.tv_sec * 1000 + end_time.tv_nsec / 1000000);
        int delta_time_ms = end_time_ms - start_time_ms;
        fprintf(stdout, "%ds %dms\n", delta_time_ms / 1000,
                delta_time_ms % 1000);
    }
    {
        float *restrict grad = calloc(logprob_nelt, sizeof(float));
        fprintf(stdout, "Timing %zu reps of crf_flipflop_grad\n", ntimes);
        struct timespec start_time;
        clock_gettime(CLOCK_MONOTONIC, &start_time);
        for (size_t i = 0; i < ntimes; i++) {
            fprintf(stdout, "Round %zu:\n", i);
            crf_flipflop_grad(logprob, ntrans, nblock, nbatch,
                              moves, stays, seqlen, sharpfact, score, grad);
            for (size_t j = 0; j < nbatch; j++) {
                fprintf(stdout, "%f\n", score[j]);
            }
        }
        struct timespec end_time;
        clock_gettime(CLOCK_MONOTONIC, &end_time);
        int start_time_ms =
            (start_time.tv_sec * 1000 + start_time.tv_nsec / 1000000);
        int end_time_ms = (end_time.tv_sec * 1000 + end_time.tv_nsec / 1000000);
        int delta_time_ms = end_time_ms - start_time_ms;
        fprintf(stdout, "%ds %dms\n", delta_time_ms / 1000,
                delta_time_ms % 1000);
        free(grad);
    }

    free(score);
    free(stays);
    free(moves);
    free(seqs);
    free(seqlen);
    free(logprob);
    return EXIT_SUCCESS;
}
#endif                          /* SPEED_TEST */
