#pragma once
#include "types.h"
#include <libretro.h>

#ifdef __cplusplus
extern "C" {
#endif

void WriteSample(s16 right, s16 left);

/* The frontend's (libretro.cpp) */
extern retro_audio_sample_batch_t audio_batch_cb;

/* The sound of the frame that just ran, sent at the end of retro_run */
void FlushAudioFrame(void);
/* What was not sent dropped (a state loaded, a reset) */
void ResetAudioBuffer(void);
/* The samples held back at the end of a frame, in a state */
void AudioHoldbackSerialize(void **data, unsigned int *total_size);
void AudioHoldbackUnserialize(void **data, unsigned int *total_size);

#ifdef __cplusplus
}
#endif
