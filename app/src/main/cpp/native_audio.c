#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <pthread.h>
#include <aaudio/AAudio.h>

#define NATIVE_AUDIO_BUFFER_SIZE 16384 // Búfer de 16KB para amortiguar ráfagas masivas
#define NATIVE_AUDIO_CHANNELS 2
#define NATIVE_AUDIO_RATE 48000

typedef struct {
    int16_t data[NATIVE_AUDIO_BUFFER_SIZE];
    int head;
    int tail;
    pthread_mutex_t mutex;
    bool is_running;
    AAudioStream *aaudio_stream;
} WrapperAudioBuffer;

static WrapperAudioBuffer *g_audio_ctx = NULL;
static pthread_t g_audio_thread;

// Hilo asíncrono nativo elástico para despacho directo
static void* wrapper_audio_playback_loop(void *arg) {
    WrapperAudioBuffer *ctx = (WrapperAudioBuffer*)arg;
    
    // 🏗️ CONSTRUCCIÓN DEL SUMIDERO EN EL HILO AUDIO:
    // Forzamos el arranque físico desde este hilo con contexto nativo legítimo.
    AAudioStreamBuilder *builder = NULL;
    if (AAudio_createStreamBuilder(&builder) == AAUDIO_OK) {
        AAudioStreamBuilder_setSampleRate(builder, NATIVE_AUDIO_RATE);
        AAudioStreamBuilder_setChannelCount(builder, NATIVE_AUDIO_CHANNELS);
        AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
        AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY); // Forzamos baja latencia MALI
        AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);

        if (AAudioStreamBuilder_openStream(builder, &ctx->aaudio_stream) == AAUDIO_OK) {
            AAudioStream_requestStart(ctx->aaudio_stream);
        }
        AAudioStreamBuilder_delete(builder);
    }

    int16_t temp_buffer[512];

    while (ctx->is_running) {
        pthread_mutex_lock(&ctx->mutex);
        
        int samples_available = (ctx->head - ctx->tail + NATIVE_AUDIO_BUFFER_SIZE) % NATIVE_AUDIO_BUFFER_SIZE;
        
        // 🔄 PACER ELÁSTICO SIN BLOQUEO:
        if (samples_available < 128) {
            pthread_mutex_unlock(&ctx->mutex);
            usleep(2000); // Pequeño respiro de 2 milisegundos para absorber picos JIT de box64
            continue;
        }

        int samples_to_play = samples_available;
        if (samples_to_play > 512) samples_to_play = 512;

        for (int i = 0; i < samples_to_play; i++) {
            temp_buffer[i] = ctx->data[ctx->tail];
            ctx->tail = (ctx->tail + 1) % NATIVE_AUDIO_BUFFER_SIZE;
        }

        pthread_mutex_unlock(&ctx->mutex);

        // 🔊 AUDIO DIRECTO AL HARDWARE REAL DESDE EL CORAZÓN DE WINE:
        if (ctx->aaudio_stream && ctx->is_running) {
            int32_t num_frames = samples_to_play / NATIVE_AUDIO_CHANNELS;
            if (num_frames > 0) {
                AAudioStream_write(ctx->aaudio_stream, temp_buffer, num_frames, 5000000);
            }
        }
    }

    // 🛑 CIERRE SEGURO DEL HARDWARE:
    if (ctx->aaudio_stream) {
        AAudioStream_requestStop(ctx->aaudio_stream);
        AAudioStream_close(ctx->aaudio_stream);
        ctx->aaudio_stream = NULL;
    }

    return NULL;
}

// 🚀 EXPOSICIÓN DE LAS PUERTAS DE CONTROL DEL DRIVER (ESTILO EXPERIMENTAL):
void wine_directaudio_init(void) {
    if (g_audio_ctx) return;

    g_audio_ctx = (WrapperAudioBuffer*)calloc(1, sizeof(WrapperAudioBuffer));
    g_audio_ctx->head = 0;
    g_audio_ctx->tail = 0;
    g_audio_ctx->is_running = true;
    g_audio_ctx->aaudio_stream = NULL;

    pthread_mutex_init(&g_audio_ctx->mutex, NULL);
    pthread_create(&g_audio_thread, NULL, wrapper_audio_playback_loop, g_audio_ctx);
}

void wine_directaudio_write(const int16_t *samples, int count) {
    if (!g_audio_ctx || !g_audio_ctx->is_running || count <= 0 || samples == NULL) return;

    pthread_mutex_lock(&g_audio_ctx->mutex);

    for (int i = 0; i < count; i++) {
        int next_head = (g_audio_ctx->head + 1) % NATIVE_AUDIO_BUFFER_SIZE;
        if (next_head == g_audio_ctx->tail) {
            g_audio_ctx->tail = (g_audio_ctx->tail + 1) % NATIVE_AUDIO_BUFFER_SIZE;
        }
        g_audio_ctx->data[g_audio_ctx->head] = samples[i];
        g_audio_ctx->head = next_head;
    }

    pthread_mutex_unlock(&g_audio_ctx->mutex);
}

void wine_directaudio_terminate(void) {
    if (!g_audio_ctx) return;

    pthread_mutex_lock(&g_audio_ctx->mutex);
    g_audio_ctx->is_running = false;
    pthread_mutex_unlock(&g_audio_ctx->mutex);

    pthread_join(g_audio_thread, NULL);

    pthread_mutex_destroy(&g_audio_ctx->mutex);
    free(g_audio_ctx);
    g_audio_ctx = NULL;
}
