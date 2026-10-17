#include "mod_audio.h"

#include <limits.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* The runner's existing APU lock serializes audio-callback mixing against
 * trusted game code issuing play/register/unregister requests. */
extern void RtlApuLock(void);
extern void RtlApuUnlock(void);

#define SNES_MOD_AUDIO_MAX_CLIPS 64
#define SNES_MOD_AUDIO_MAX_VOICES 8
/* Keep hostile/accidental trusted-plugin registration bounded too. */
#define SNES_MOD_AUDIO_MAX_TOTAL_BYTES (16u * 1024u * 1024u)

typedef struct ClipSlot {
    int16_t *samples;
    uint32_t frame_count;
    uint32_t sample_rate;
    uint32_t channels;
} ClipSlot;

typedef struct Voice {
    SNESModAudioClip clip;
    uint64_t phase; /* source-frame position in unsigned 32.32 fixed point */
    int gain_percent;
    int pitch_q10;   /* source-rate multiplier, 1024 = unity */
    int looping;
    uint32_t loop_start;
    uint32_t loop_end; /* exclusive */
    uint32_t generation; /* distinguishes reuse of the same slot */
} Voice;

static ClipSlot s_clips[SNES_MOD_AUDIO_MAX_CLIPS];
static Voice s_voices[SNES_MOD_AUDIO_MAX_VOICES];
static unsigned s_replace_cursor;
static size_t s_clip_bytes;
static uint32_t s_voice_generation;
static uint32_t s_reset_generation;
static ClipSlot *clip_slot(SNESModAudioClip clip);

/* Handle = (generation << 4) | slot + 1, generation masked so the int handle
 * stays positive. */
static SNESModAudioVoice voice_handle(int slot) {
    return (SNESModAudioVoice)(((s_voices[slot].generation & 0x07ffffffu) << 4) |
                               (unsigned)(slot + 1));
}

static Voice *voice_from_handle(SNESModAudioVoice handle) {
    if (handle <= 0) return NULL;
    const int slot = (int)((unsigned)handle & 0xfu) - 1;
    if (slot < 0 || slot >= SNES_MOD_AUDIO_MAX_VOICES) return NULL;
    Voice *voice = &s_voices[slot];
    if (voice->clip == SNES_MOD_AUDIO_CLIP_INVALID) return NULL;
    if ((voice->generation & 0x07ffffffu) != ((unsigned)handle >> 4)) return NULL;
    return voice;
}

static int allocate_voice_locked(void) {
    int index;
    for (index = 0; index < SNES_MOD_AUDIO_MAX_VOICES; ++index)
        if (s_voices[index].clip == SNES_MOD_AUDIO_CLIP_INVALID) break;
    if (index == SNES_MOD_AUDIO_MAX_VOICES) {
        /* Prefer stealing a one-shot over a loop. */
        for (unsigned probe = 0; probe < SNES_MOD_AUDIO_MAX_VOICES; ++probe) {
            const unsigned candidate =
                (s_replace_cursor + probe) % SNES_MOD_AUDIO_MAX_VOICES;
            if (!s_voices[candidate].looping) {
                index = (int)candidate;
                s_replace_cursor = (candidate + 1u) % SNES_MOD_AUDIO_MAX_VOICES;
                break;
            }
        }
        if (index == SNES_MOD_AUDIO_MAX_VOICES) {
            index = (int)s_replace_cursor;
            s_replace_cursor = (s_replace_cursor + 1u) % SNES_MOD_AUDIO_MAX_VOICES;
        }
    }
    memset(&s_voices[index], 0, sizeof(Voice));
    s_voices[index].generation = ++s_voice_generation;
    s_voices[index].pitch_q10 = 1024;
    return index;
}

static SNESModAudioVoice start_voice(SNESModAudioClip clip, int gain_percent,
                                     int looping, uint32_t loop_start,
                                     uint32_t loop_end) {
    RtlApuLock();
    ClipSlot *slot = clip_slot(clip);
    if (!slot) {
        RtlApuUnlock();
        return SNES_MOD_AUDIO_VOICE_INVALID;
    }
    if (gain_percent < 0) gain_percent = 0;
    if (gain_percent > 200) gain_percent = 200;
    if (looping) {
        if (loop_end == 0 || loop_end > slot->frame_count)
            loop_end = slot->frame_count;
        if (loop_start >= loop_end) loop_start = 0;
        if (loop_end - loop_start < 2) {
            RtlApuUnlock();
            return SNES_MOD_AUDIO_VOICE_INVALID;
        }
    }
    const int index = allocate_voice_locked();
    s_voices[index].clip = clip;
    s_voices[index].phase = 0;
    s_voices[index].gain_percent = gain_percent;
    s_voices[index].looping = looping;
    s_voices[index].loop_start = loop_start;
    s_voices[index].loop_end = loop_end;
    const SNESModAudioVoice handle = voice_handle(index);
    RtlApuUnlock();
    return handle;
}

static ClipSlot *clip_slot(SNESModAudioClip clip) {
    if (clip <= 0 || clip > SNES_MOD_AUDIO_MAX_CLIPS) return NULL;
    if (!s_clips[clip - 1].samples) return NULL;
    return &s_clips[clip - 1];
}

/* The streamed voice. buf holds source frames [0, len); idx/frac is the read
 * position. Interpolating needs idx and idx+1, so a refill keeps the current
 * frame at buf[0] and appends after it. */
#define SNES_MOD_AUDIO_STREAM_BUF 4096
typedef struct StreamVoice {
    SNESModAudioStreamFill fill;
    void *user;
    uint32_t sample_rate;
    uint32_t channels;
    int gain_percent;
    int active;
    int ended;
    int source_done;
    uint32_t len;
    uint32_t idx;
    uint32_t frac; /* 0.32 fixed point between idx and idx+1 */
    int16_t buf[SNES_MOD_AUDIO_STREAM_BUF * 2];
} StreamVoice;
static StreamVoice s_stream;

static void stream_stop_locked(void) {
    s_stream.active = 0;
    s_stream.fill = NULL;
    s_stream.user = NULL;
    s_stream.len = s_stream.idx = s_stream.frac = 0;
    s_stream.source_done = 0;
}

static void stop_all_locked(void) {
    memset(s_voices, 0, sizeof(s_voices));
    s_replace_cursor = 0;
    stream_stop_locked();
    s_stream.ended = 0;
}

SNESModAudioClip snes_mod_audio_register_pcm_s16(
    const int16_t *samples, uint32_t frame_count, uint32_t sample_rate,
    uint32_t channels) {
    if (!samples || frame_count == 0 || sample_rate < 8000 ||
        sample_rate > 192000 ||
        (channels != SNES_MOD_AUDIO_CHANNELS_MONO &&
         channels != SNES_MOD_AUDIO_CHANNELS_STEREO) ||
        frame_count > SIZE_MAX / ((size_t)channels * sizeof(*samples)))
        return SNES_MOD_AUDIO_CLIP_INVALID;

    const size_t bytes = (size_t)frame_count * channels * sizeof(*samples);
    if (bytes > SNES_MOD_AUDIO_MAX_TOTAL_BYTES)
        return SNES_MOD_AUDIO_CLIP_INVALID;

    RtlApuLock();
    if (s_clip_bytes > SNES_MOD_AUDIO_MAX_TOTAL_BYTES - bytes) {
        RtlApuUnlock();
        return SNES_MOD_AUDIO_CLIP_INVALID;
    }
    int index;
    for (index = 0; index < SNES_MOD_AUDIO_MAX_CLIPS; ++index)
        if (!s_clips[index].samples) break;
    if (index == SNES_MOD_AUDIO_MAX_CLIPS) {
        RtlApuUnlock();
        return SNES_MOD_AUDIO_CLIP_INVALID;
    }
    int16_t *copy = (int16_t *)malloc(bytes);
    if (!copy) {
        RtlApuUnlock();
        return SNES_MOD_AUDIO_CLIP_INVALID;
    }
    memcpy(copy, samples, bytes);
    s_clips[index].samples = copy;
    s_clips[index].frame_count = frame_count;
    s_clips[index].sample_rate = sample_rate;
    s_clips[index].channels = channels;
    s_clip_bytes += bytes;
    RtlApuUnlock();
    return index + 1;
}

void snes_mod_audio_unregister(SNESModAudioClip clip) {
    RtlApuLock();
    ClipSlot *slot = clip_slot(clip);
    if (slot) {
        for (int i = 0; i < SNES_MOD_AUDIO_MAX_VOICES; ++i)
            if (s_voices[i].clip == clip) memset(&s_voices[i], 0, sizeof(Voice));
        s_clip_bytes -= (size_t)slot->frame_count * slot->channels *
                        sizeof(*slot->samples);
        free(slot->samples);
        memset(slot, 0, sizeof(*slot));
    }
    RtlApuUnlock();
}

int snes_mod_audio_play(SNESModAudioClip clip, int gain_percent) {
    return start_voice(clip, gain_percent, 0, 0, 0) != SNES_MOD_AUDIO_VOICE_INVALID;
}

SNESModAudioVoice snes_mod_audio_play_voice(SNESModAudioClip clip,
                                            int gain_percent) {
    return start_voice(clip, gain_percent, 0, 0, 0);
}

SNESModAudioVoice snes_mod_audio_play_loop(SNESModAudioClip clip,
                                           int gain_percent,
                                           uint32_t loop_start_frame,
                                           uint32_t loop_end_frame) {
    return start_voice(clip, gain_percent, 1, loop_start_frame, loop_end_frame);
}

int snes_mod_audio_set_voice_gain(SNESModAudioVoice voice, int gain_percent) {
    RtlApuLock();
    Voice *v = voice_from_handle(voice);
    if (v) {
        if (gain_percent < 0) gain_percent = 0;
        if (gain_percent > 200) gain_percent = 200;
        v->gain_percent = gain_percent;
    }
    RtlApuUnlock();
    return v != NULL;
}

int snes_mod_audio_set_voice_pitch(SNESModAudioVoice voice, int pitch_q10) {
    RtlApuLock();
    Voice *v = voice_from_handle(voice);
    if (v) {
        if (pitch_q10 < 256) pitch_q10 = 256;
        if (pitch_q10 > 4096) pitch_q10 = 4096;
        v->pitch_q10 = pitch_q10;
    }
    RtlApuUnlock();
    return v != NULL;
}

void snes_mod_audio_stop_voice(SNESModAudioVoice voice) {
    RtlApuLock();
    Voice *v = voice_from_handle(voice);
    if (v) memset(v, 0, sizeof(*v));
    RtlApuUnlock();
}

int snes_mod_audio_voice_active(SNESModAudioVoice voice) {
    RtlApuLock();
    const int active = voice_from_handle(voice) != NULL;
    RtlApuUnlock();
    return active;
}

void snes_mod_audio_stop_all(void) {
    RtlApuLock();
    stop_all_locked();
    s_reset_generation++;
    RtlApuUnlock();
}
uint32_t snes_mod_audio_reset_generation(void) {
    /* Queried on the guest thread, including from a port-read observer while
     * the APU lock is already held. stop_all also runs on the guest thread. */
    return s_reset_generation;
}

static int32_t interpolated_sample(const ClipSlot *slot, uint32_t frame,
                                   uint32_t fraction, uint32_t channel) {
    const uint32_t source_channel = slot->channels == 1 ? 0 : channel;
    const size_t first = (size_t)frame * slot->channels + source_channel;
    const int32_t s0 = slot->samples[first];
    const int32_t s1 = frame + 1 < slot->frame_count
        ? slot->samples[first + slot->channels] : s0;
    return s0 + (int32_t)(((int64_t)(s1 - s0) * fraction) / 4294967296LL);
}

static int32_t sample_for_output(const ClipSlot *slot, uint32_t frame,
                                 uint32_t fraction, uint32_t output_channel,
                                 uint32_t output_channels) {
    if (output_channels == 1 && slot->channels == 2) {
        const int32_t left = interpolated_sample(slot, frame, fraction, 0);
        const int32_t right = interpolated_sample(slot, frame, fraction, 1);
        return (left + right) / 2;
    }
    return interpolated_sample(slot, frame, fraction, output_channel);
}

static int16_t clamp_s16(int64_t value) {
    if (value > INT16_MAX) return INT16_MAX;
    if (value < INT16_MIN) return INT16_MIN;
    return (int16_t)value;
}

int snes_mod_audio_stream_start(SNESModAudioStreamFill fill, void *user,
                                uint32_t sample_rate, uint32_t channels,
                                int gain_percent) {
    if (!fill || sample_rate < 8000 || sample_rate > 192000 ||
        (channels != SNES_MOD_AUDIO_CHANNELS_MONO &&
         channels != SNES_MOD_AUDIO_CHANNELS_STEREO))
        return 0;
    if (gain_percent < 0) gain_percent = 0;
    if (gain_percent > 200) gain_percent = 200;
    RtlApuLock();
    stream_stop_locked();
    s_stream.fill = fill;
    s_stream.user = user;
    s_stream.sample_rate = sample_rate;
    s_stream.channels = channels;
    s_stream.gain_percent = gain_percent;
    s_stream.ended = 0;
    s_stream.active = 1;
    RtlApuUnlock();
    return 1;
}

void snes_mod_audio_stream_stop(void) {
    RtlApuLock();
    stream_stop_locked();
    s_stream.ended = 0;
    RtlApuUnlock();
}

int snes_mod_audio_stream_set_gain(int gain_percent) {
    if (gain_percent < 0) gain_percent = 0;
    if (gain_percent > 200) gain_percent = 200;
    RtlApuLock();
    const int active = s_stream.active;
    s_stream.gain_percent = gain_percent;
    RtlApuUnlock();
    return active;
}

int snes_mod_audio_stream_active(void) {
    RtlApuLock();
    const int active = s_stream.active;
    RtlApuUnlock();
    return active;
}

int snes_mod_audio_stream_ended(void) {
    RtlApuLock();
    const int ended = s_stream.ended;
    RtlApuUnlock();
    return ended;
}

/* Make buf[idx] and buf[idx + 1] valid, refilling from the source. Returns 0
 * once there is no next frame left (the stream has ended). */
static int stream_ensure_locked(void) {
    while (s_stream.idx + 1 >= s_stream.len) {
        if (s_stream.source_done)
            return s_stream.idx + 1 < s_stream.len;
        const uint32_t ch = s_stream.channels;
        uint32_t keep = 0;
        if (s_stream.idx < s_stream.len) {
            memmove(s_stream.buf, s_stream.buf + (size_t)s_stream.idx * ch,
                    (size_t)ch * sizeof(int16_t));
            keep = 1;
        }
        s_stream.idx = 0;
        const uint32_t want = SNES_MOD_AUDIO_STREAM_BUF - keep;
        const uint32_t got = s_stream.fill(s_stream.user,
                                           s_stream.buf + (size_t)keep * ch, want);
        s_stream.len = keep + (got > want ? want : got);
        if (got < want) s_stream.source_done = 1;
        if (keep == 0 && s_stream.len == 0) return 0;
    }
    return 1;
}

static void stream_mix_locked(int64_t mixed[2], uint32_t output_rate,
                              uint32_t output_channels) {
    if (!s_stream.active) return;
    if (!stream_ensure_locked()) {
        stream_stop_locked();
        s_stream.ended = 1;
        return;
    }
    const uint32_t ch = s_stream.channels;
    const int16_t *a = s_stream.buf + (size_t)s_stream.idx * ch;
    const int16_t *b = a + ch;
    int32_t out[2];
    for (uint32_t c = 0; c < 2; ++c) {
        const uint32_t sc = ch == 1 ? 0 : c;
        out[c] = a[sc] + (int32_t)(((int64_t)(b[sc] - a[sc]) * s_stream.frac) /
                                   4294967296LL);
    }
    if (output_channels == 1) {
        mixed[0] += ((int64_t)((out[0] + out[1]) / 2) * s_stream.gain_percent) / 100;
    } else {
        mixed[0] += ((int64_t)out[0] * s_stream.gain_percent) / 100;
        mixed[1] += ((int64_t)out[1] * s_stream.gain_percent) / 100;
    }
    const uint64_t step = ((uint64_t)s_stream.sample_rate << 32) / output_rate;
    const uint64_t pos = (uint64_t)s_stream.frac + step;
    s_stream.idx += (uint32_t)(pos >> 32);
    s_stream.frac = (uint32_t)pos;
}

void snes_mod_audio_mix(int16_t *dst, int frame_count, uint32_t output_rate,
                        uint32_t output_channels) {
    if (!dst || frame_count <= 0 || output_rate < 8000 || output_rate > 192000 ||
        (output_channels != 1 && output_channels != 2))
        return;
    for (int frame = 0; frame < frame_count; ++frame) {
        int64_t mixed[2] = { dst[(size_t)frame * output_channels], 0 };
        if (output_channels == 2) mixed[1] = dst[(size_t)frame * 2 + 1];
        for (int voice_index = 0; voice_index < SNES_MOD_AUDIO_MAX_VOICES;
             ++voice_index) {
            Voice *voice = &s_voices[voice_index];
            if (voice->clip == SNES_MOD_AUDIO_CLIP_INVALID) continue;
            ClipSlot *slot = clip_slot(voice->clip);
            const uint32_t source_frame = (uint32_t)(voice->phase >> 32);
            if (!slot || source_frame >= slot->frame_count) {
                memset(voice, 0, sizeof(*voice));
                continue;
            }
            const uint32_t fraction = (uint32_t)voice->phase;
            for (uint32_t channel = 0; channel < output_channels; ++channel) {
                const int32_t sample = sample_for_output(
                    slot, source_frame, fraction, channel, output_channels);
                mixed[channel] += ((int64_t)sample * voice->gain_percent) / 100;
            }
            const uint64_t step =
                (((uint64_t)slot->sample_rate << 32) / output_rate) *
                (uint64_t)(voice->pitch_q10 > 0 ? voice->pitch_q10 : 1024) /
                1024u;
            if (voice->phase > UINT64_MAX - step)
                memset(voice, 0, sizeof(*voice));
            else
                voice->phase += step;
            if (voice->clip == SNES_MOD_AUDIO_CLIP_INVALID) continue;
            if (voice->looping) {
                if ((voice->phase >> 32) >= voice->loop_end) {
                    const uint64_t span =
                        ((uint64_t)(voice->loop_end - voice->loop_start)) << 32;
                    voice->phase -= span;
                    if ((voice->phase >> 32) < voice->loop_start ||
                        (voice->phase >> 32) >= voice->loop_end)
                        voice->phase = (uint64_t)voice->loop_start << 32;
                }
            } else if ((voice->phase >> 32) >= slot->frame_count) {
                memset(voice, 0, sizeof(*voice));
            }
        }
        stream_mix_locked(mixed, output_rate, output_channels);
        for (uint32_t channel = 0; channel < output_channels; ++channel)
            dst[(size_t)frame * output_channels + channel] = clamp_s16(mixed[channel]);
    }
}
