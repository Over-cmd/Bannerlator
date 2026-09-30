package com.winlator.star.core;

public class NativeAudio {
    // 🚀 ENLACE NATIVO MODULAR AISLADO:
    // Mapeamos los métodos estáticos para que enganchen con las firmas JNI 
    // que programamos al final de tu archivo native_audio.c
    public native static void init();
    public native static void write(short[] samples, int count);
    public native static void terminate();

    static {
        // 🔊 JAQUE MATE AL SILENCIO:
        // Cargamos estrictamente el módulo independiente 'native_audio' creado por tu CMakeLists.txt
        System.loadLibrary("native_audio");
    }
}
