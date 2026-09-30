package com.winlator.star.alsaserver;

import com.winlator.star.sysvshm.SysVSharedMemory;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;

public class ALSAClient {
    public enum DataType {
        U8(1), S16LE(2), S16BE(2), FLOATLE(4), FLOATBE(4);
        public final byte byteCount;

        DataType(int byteCount) {
            this.byteCount = (byte)byteCount;
        }
    }
    private DataType dataType = DataType.U8;
    private byte channelCount = 2;
    private int sampleRate = 0;
    private int position;
    private int bufferSize;
    private int frameBytes;
    private ByteBuffer sharedBuffer;
    private boolean playing = false;
    private long streamPtr = 0;

    static {
        System.loadLibrary("winlator");
    }

    public void release() {
        // 🛑 CIERRE COMPLETO DEL HARDWARE DEL WRAPPER:
        // Apagamos y destruimos el bucle asíncrono en tu libvulkan_wrapper.so
        try {
            com.winlator.star.core.NativeAudio.terminate();
        } catch (Throwable e) {
            // Mitigación por si el componente se invoca sin la librería cargada
        }

        if (sharedBuffer != null) {
            SysVSharedMemory.unmapSHMSegment(sharedBuffer, sharedBuffer.capacity());
            sharedBuffer = null;
        }

        if (streamPtr > 0) {
            stop(streamPtr);
            close(streamPtr);
        }
        playing = false;
        streamPtr = 0;
    }

    public void prepare() {
        position = 0;
        frameBytes = channelCount * dataType.byteCount;
        release();

        // 🚀 INICIALIZACIÓN NATIVA DEL WRAPPER SEGURO:
        // Arrancamos tu bucle asíncrono de AAudio en C, pero NO detenemos el flujo
        // para que Java mantenga la estructura del stream viva y el emulador no se caiga.
        try {
            com.winlator.star.core.NativeAudio.init();
        } catch (Throwable e) {
            // Fallback por si la librería no carga
        }

        if (!isValidBufferSize()) return;

        // Forzamos la creación del stream nativo de control para estabilizar el proceso
        streamPtr = create(dataType.ordinal(), channelCount, sampleRate, bufferSize);
        if (streamPtr > 0) start();
    }

    public void start() {
        if (streamPtr > 0 && !playing) {
            start(streamPtr);
            playing = true;
        }
    }

    public void stop() {
        if (streamPtr > 0 && playing) {
            stop(streamPtr);
            playing = false;
        }
    }

    public void pause() {
        if (streamPtr > 0) {
            pause(streamPtr);
            playing = false;
        }
    }

    public void drain() {
        if (streamPtr > 0) flush(streamPtr);
    }

    public void writeDataToStream(ByteBuffer data) {
        if (dataType == DataType.S16LE || dataType == DataType.FLOATLE) {
            data.order(ByteOrder.LITTLE_ENDIAN);
        }
        else if (dataType == DataType.S16BE || dataType == DataType.FLOATBE) {
            data.order(ByteOrder.BIG_ENDIAN);
        }

        // 🔊 COMPROBACIÓN DE SEGURIDAD MAESTRA NATIVEAUDIO:
        // Evitamos que ráfagas desalineadas de Wine hagan explotar la RAM del teléfono.
        if (playing && streamPtr == 0) {
            try {
                int remainingBytes = data.remaining();
                // Forzamos la alineación estricta a 16 bits (múltiplos de 2 bytes por muestra)
                int shortCount = remainingBytes / 2;
                
                if (shortCount > 0 && (remainingBytes % 2 == 0)) {
                    short[] samples = new short[shortCount];
                    // Usamos una lectura segura por posición absoluta para no estresar el ShortBuffer
                    for (int i = 0; i < shortCount; i++) {
                        samples[i] = data.getShort();
                    }
                    com.winlator.star.core.NativeAudio.write(samples, shortCount);
                    position += (shortCount / channelCount);
                } else {
                    // Si el buffer viene corrupto o impar, vaciamos el puntero de largo de forma elástica
                    data.position(data.limit());
                }
                return;
            } catch (Throwable e) {
                // Si ocurre cualquier amago de desborde, forzamos el avance para que el emulador no muera
                data.position(data.limit());
                return;
            }
        }

        if (playing && streamPtr > 0) {
            int numFrames = data.limit() / frameBytes;
            int framesWritten = write(streamPtr, data, numFrames);
            if (framesWritten > 0) position += framesWritten;
            data.rewind();
        }
    }

    public int pointer() {
        return position;
    }

    public void setDataType(DataType dataType) {
        this.dataType = dataType;
    }

    public void setChannelCount(int channelCount) {
        this.channelCount = (byte)channelCount;
    }

    public void setSampleRate(int sampleRate) {
        this.sampleRate = sampleRate;
    }

    public void setBufferSize(int bufferSize) {
        this.bufferSize = bufferSize;
    }

    public ByteBuffer getSharedBuffer() {
        return sharedBuffer;
    }

    public void setSharedBuffer(ByteBuffer sharedBuffer) {
        this.sharedBuffer = sharedBuffer;
    }

    public DataType getDataType() {
        return dataType;
    }

    public byte getChannelCount() {
        return channelCount;
    }

    public int getSampleRate() {
        return sampleRate;
    }

    public int getBufferSize() {
        return bufferSize;
    }

    public int getBufferSizeInBytes() {
        return bufferSize * frameBytes;
    }

    private boolean isValidBufferSize() {
        return (getBufferSizeInBytes() % frameBytes == 0) && bufferSize > 0;
    }

    public int computeLatencyMillis() {
        return (int)(((float)bufferSize / sampleRate) * 1000);
    }

    /**
     * Push the adaptive-audio config to the native ALSA player (process-global; read at stream open).
     * perfMode: 0=NONE, 1=LOW_LATENCY, 2=POWER_SAVING. adaptive: 1=grow buffer on underruns.
     * bufferTarget/maxBuffer: frames, 0 = auto/device-capacity. Bumps a generation counter so any live
     * stream reopens on its next write() — lets the in-game AUDIO tab apply without a relaunch.
     */
    public static native void nativeSetAudioConfig(int perfMode, int adaptive, int bufferTarget, int maxBuffer);

    private native long create(int format, byte channelCount, int sampleRate, int bufferSize);

    private native int write(long streamPtr, ByteBuffer buffer, int numFrames);

    private native void start(long streamPtr);

    private native void stop(long streamPtr);

    private native void pause(long streamPtr);

    private native void flush(long streamPtr);

    private native void close(long streamPtr);
}
