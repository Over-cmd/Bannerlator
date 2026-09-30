package com.winlator.star.core;

import android.content.Context;

public class NativeAudio {
    // 🚀 FIRMAS JNI EXCLUSIVAS DEL WRAPPER:
    // Mapeamos los métodos nativos para que enganchen con las funciones de C++
    // de tu repositorio gamenative-wrapper.
    public native static void init();
    public native static void write(short[] samples, int count);
    public native static void terminate();

    static {
        // Forzamos al cargador dinámico de Android a enlazar tu driver gráfico
        System.loadLibrary("vulkan_wrapper");
    }
}
