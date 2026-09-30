#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <pthread.h>
#include <jni.h>
#include <aaudio/AAudio.h> // 🚀 CONEXIÓN FÍSICA: Cargamos la API AAudio de baja latencia de Android

#define NATIVE_AUDIO_BUFFER_SIZE 8192
#define NATIVE_AUDIO_CHANNELS 2
#define NATIVE_AUDIO_RATE 48000

void wrapper_native_audio_init(void);
void wrapper_native_audio_write(const int16_t *samples, int count);
void wrapper_native_audio_terminate(void);

typedef struct {
    int16_t data[NATIVE_AUDIO_BUFFER_SIZE];
    int head;
    int tail;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    bool is_running;
    AAudioStream *aaudio_stream; // 🎛️ MANEJADOR DE HARDWARE: Puntero del flujo físico de Android
} WrapperAudioBuffer;

static WrapperAudioBuffer *g_audio_ctx = NULL;
static pthread_t g_audio_thread;

// Hilo asíncrono nativo de alta prioridad para despacho directo
static void* wrapper_audio_playback_loop(void *arg) {
    WrapperAudioBuffer *ctx = (WrapperAudioBuffer*)arg;
    
    struct sched_param param;
    param.sched_priority = sched_get_priority_max(SCHED_FIFO);
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);

    // Búfer local de transferencia temporal para el silicio
    int16_t temp_buffer[512];

    while (ctx->is_running) {
        pthread_mutex_lock(&ctx->mutex);
        
        while (ctx->head == ctx->tail && ctx->is_running) {
            pthread_cond_wait(&ctx->cond, &ctx->mutex);
        }

        if (!ctx->is_running) {
            pthread_mutex_unlock(&ctx->mutex);
            break;
        }

        int samples_to_play = (ctx->head - ctx->tail + NATIVE_AUDIO_BUFFER_SIZE) % NATIVE_AUDIO_BUFFER_SIZE;
        if (samples_to_play > 512) samples_to_play = 512;

        // Volcamos los bytes desde el búfer circular al búfer temporal local
        for (int i = 0; i < samples_to_play; i++) {
            temp_buffer[i] = ctx->data[ctx->tail];
            ctx->tail = (ctx->tail + 1) % NATIVE_AUDIO_BUFFER_SIZE;
        }

        pthread_mutex_unlock(&ctx->mutex);

        // 🔊 JAKE MATE AL SILENCIO: Inyectamos el sonido directamente en la GPU/Hardware de audio
        if (ctx->aaudio_stream && samples_to_play > 0) {
            // Calculamos el número de frames (Cada frame estéreo tiene 2 muestras)
            int32_t num_frames = samples_to_play / NATIVE_AUDIO_CHANNELS;
            if (num_frames > 0) {
                // Escribimos los datos en modo bloqueante de baja latencia con un timeout de 10ms
                AAudioStream_write(ctx->aaudio_stream, temp_buffer, num_frames, 10000000);
            }
        }
    }
    return NULL;
}

// Inicializador oficial del puente de sonido nativo
void wrapper_native_audio_init(void) {
    if (g_audio_ctx) return;

    g_audio_ctx = (WrapperAudioBuffer*)calloc(1, sizeof(WrapperAudioBuffer));
    g_audio_ctx->head = 0;
    g_audio_ctx->tail = 0;
    g_audio_ctx->is_running = true;
    g_audio_ctx->aaudio_stream = NULL;

    // 🏗️ CONSTRUCCIÓN DEL SUMIDERO AAUDIO:
    AAudioStreamBuilder *builder = NULL;
    if (AAudio_createStreamBuilder(&builder) == AAUDIO_OK) {
        AAudioStreamBuilder_setSampleRate(builder, NATIVE_AUDIO_RATE);
        AAudioStreamBuilder_setChannelCount(builder, NATIVE_AUDIO_CHANNELS);
        AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
        AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY); // Modo Ultra-Baja Latencia
        AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);

        if (AAudioStreamBuilder_openStream(builder, &g_audio_ctx->aaudio_stream) == AAUDIO_OK) {
            AAudioStream_requestStart(g_audio_ctx->aaudio_stream);
        }
        AAudioStreamBuilder_delete(builder);
    }

    pthread_mutex_init(&g_audio_ctx->mutex, NULL);
    pthread_cond_init(&g_audio_ctx->cond, NULL);

    pthread_create(&g_audio_thread, NULL, wrapper_audio_playback_loop, g_audio_ctx);
}

void wrapper_native_audio_write(const int16_t *samples, int count) {
    if (!g_audio_ctx || !g_audio_ctx->is_running || count <= 0) return;

    // 🚀 COJÍN DE SEGURIDAD MUTEX: Bloqueamos los hilos antes de tocar la RAM
    pthread_mutex_lock(&g_audio_ctx->mutex);

    for (int i = 0; i < count; i++) {
        int next_head = (g_audio_ctx->head + 1) % NATIVE_AUDIO_BUFFER_SIZE;
        
        if (next_head == g_audio_ctx->tail) {
            // Buffer lleno (Underrun guard): descartamos muestras viejas para evitar saturación metálica
            g_audio_ctx->tail = (g_audio_ctx->tail + 1) % NATIVE_AUDIO_BUFFER_SIZE;
        }
        
        // Copia atómica directa al bloque de memoria seguro asignado por malloc
        g_audio_ctx->data[g_audio_ctx->head] = samples[i];
        g_audio_ctx->head = next_head;
    }

    // Despertamos al hilo de reproducción de AAudio de forma segura
    pthread_cond_signal(&g_audio_ctx->cond);
    pthread_mutex_unlock(&g_audio_ctx->mutex);
}

void wrapper_native_audio_terminate(void) {
    if (!g_audio_ctx) return;

    pthread_mutex_lock(&g_audio_ctx->mutex);
    g_audio_ctx->is_running = false;
    pthread_cond_signal(&g_audio_ctx->cond);
    pthread_mutex_unlock(&g_audio_ctx->mutex);

    pthread_join(g_audio_thread, NULL);

    if (g_audio_ctx->aaudio_stream) {
        AAudioStream_requestStop(g_audio_ctx->aaudio_stream);
        AAudioStream_close(g_audio_ctx->aaudio_stream);
    }

    pthread_mutex_destroy(&g_audio_ctx->mutex);
    pthread_cond_destroy(&g_audio_ctx->cond);
    free(g_audio_ctx);
    g_audio_ctx = NULL;
}

// =============================================================================
// 🚀 PUENTE DE COMUNICACIÓN JNI ENLACE DIRECTO (JAVA -> C):
// Vinculamos de forma estricta los métodos con el paquete com.winlator.star.core
// =============================================================================

JNIEXPORT void JNICALL
Java_com_winlator_star_core_NativeAudio_init(JNIEnv *env, jclass clazz) {
    wrapper_native_audio_init();
}

JNIEXPORT void JNICALL
Java_com_winlator_star_core_NativeAudio_write(JNIEnv *env, jclass clazz, jshortArray samples, jint count) {
    if (count <= 0) return;
    
    // Obtenemos un puntero directo de memoria a los elementos del array de Java
    jshort *body = (*env)->GetShortArrayElements(env, samples, NULL);
    if (body != NULL) {
        wrapper_native_audio_write((const int16_t*)body, count);
        // Liberamos los recursos del array sin copiar de vuelta (JNI_ABORT es más rápido)
        (*env)->ReleaseShortArrayElements(env, samples, body, JNI_ABORT);
    }
}

JNIEXPORT void JNICALL
Java_com_winlator_star_core_NativeAudio_terminate(JNIEnv *env, jclass clazz) {
    wrapper_native_audio_terminate();
}
