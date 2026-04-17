#ifndef RSP_AUDIO_H
#define RSP_AUDIO_H

#include <stdint.h>

/* Initialize RSP audio overlay (call after rspq_init) */
void rsp_audio_init(void);

/* Queue async mono->stereo mix+output on RSP.
 * src: 16-bit mono PCM in RDRAM (cache-flushed)
 * dst: 16-bit stereo interleaved output in RDRAM
 * nsamples: number of mono samples to process */
void rsp_audio_push(int16_t *src, int16_t *dst, int nsamples);

#endif
