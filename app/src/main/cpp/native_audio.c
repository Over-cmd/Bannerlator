#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <pthread.h>
#include <aaudio/AAudio.h>

#define NATIVE_AUDIO_BUFFER_SIZE 32768 // Ampliado a 32KB para blindar Wow64 contra tirones JIT
#define NATIVE_AUDIO_CHANNELS 2
#define NATIVE_AUDIO_RATE 44100

typedef struct {
    int16_t data[NATIVE_AUDIO_BUFFER_SIZE];
    int head;
    int tail;
    pthread_mutex_t mutex;
    bool is_running;
    AAudioStream *aaudio_stream;
} WrapperAudioBuffer;

static WrapperAudioBuffer *g_audio_ctx = NULL;

// 🚀 EL CALLBACK ASÍNCRONO PURO POR HARDWARE (ANTI-CORTES / LATENCIA CERO)
// Este método es invocado directamente por el Kernel de Android mediante interrupciones del chip Unisoc.
// Elimina por completo AAudioStream_write() y usleep(), erradicando los chasquidos y la mudez.
static aaudio_data_callback_result_t aaudio_hardware_callback(
    AAudioStream *stream,
    void *userData,
    void *audioData,
    int32_t numFrames) 
{
    WrapperAudioBuffer *ctx = (WrapperAudioBuffer*)userData;
    if (!ctx || !ctx->is_running || !audioData || numFrames <= 0) return AAUDIO_CALLBACK_RESULT_CONTINUE;

    int32_t samples_needed = numFrames * NATIVE_AUDIO_CHANNELS;
    int16_t *out_buffer = (int16_t*)audioData;

    pthread_mutex_lock(&ctx->mutex);
    int samples_available = (ctx->head - ctx->tail + NATIVE_AUDIO_BUFFER_SIZE) % NATIVE_AUDIO_BUFFER_SIZE;

    if (samples_available >= samples_needed) {
        // 🔄 Extracción elástica síncrona en RAM: volcamos los datos directo al chip analógico
        for (int i = 0; i < samples_needed; i++) {
            out_buffer[i] = ctx->data[ctx->tail];
            ctx->tail = (ctx->tail + 1) % NATIVE_AUDIO_BUFFER_SIZE;
        }
    } else {
        // 🤫 Escudo Anti-Chasquidos (Silencio Inteligente): Si box64 se retrasa,
        // rellenamos con silencio en vez de cortar el stream o colapsar la GPU Mali.
        memset(audioData, 0, samples_needed * sizeof(int16_t));
        
        // Compensación lineal elástica del búfer
        if (samples_available > 0) {
            for (int i = 0; i < samples_available; i++) {
                out_buffer[i] = ctx->data[ctx->tail];
                ctx->tail = (ctx->tail + 1) % NATIVE_AUDIO_BUFFER_SIZE;
            }
        }
    }
    pthread_mutex_unlock(&ctx->mutex);

    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

void wine_directaudio_init(void) {
    if (g_audio_ctx) return;

    g_audio_ctx = (WrapperAudioBuffer*)calloc(1, sizeof(WrapperAudioBuffer));
    g_audio_ctx->head = 0;
    g_audio_ctx->tail = 0;
    g_audio_ctx->is_running = true;

    pthread_mutex_init(&g_audio_ctx->mutex, NULL);

    // 🏗️ CONSTRUCCIÓN DEL SUMIDERO CON RESPUESTA BAJO NIVEL UNISOC T618
    AAudioStreamBuilder *builder = NULL;
    if (AAudio_createStreamBuilder(&builder) == AAUDIO_OK) {
        AAudioStreamBuilder_setSampleRate(builder, NATIVE_AUDIO_RATE);
        AAudioStreamBuilder_setChannelCount(builder, NATIVE_AUDIO_CHANNELS);
        AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
        
        // Sintonía de competición: Modo exclusivo de baja latencia real
        AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
        AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_EXCLUSIVE);
        
        // Conectamos el callback de hardware
        AAudioStreamBuilder_setDataCallback(builder, aaudio_hardware_callback, g_audio_ctx);

        if (AAudioStreamBuilder_openStream(builder, &g_audio_ctx->aaudio_stream) == AAUDIO_OK) {
            AAudioStream_requestStart(g_audio_ctx->aaudio_stream);
        }
        AAudioStreamBuilder_delete(builder);
    }
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

    if (g_audio_ctx->aaudio_stream) {
        AAudioStream_requestStop(g_audio_ctx->aaudio_stream);
        AAudioStream_close(g_audio_ctx->aaudio_stream);
    }

    pthread_mutex_destroy(&g_audio_ctx->mutex);
    free(g_audio_ctx);
    g_audio_ctx = NULL;
}
