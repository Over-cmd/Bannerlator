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

        // 🚀 INICIALIZACIÓN NATIVA DEL WRAPPER:
        // Si el stream nativo está activo, arrancamos el bucle de AAudio en el .so gráfico
        try {
            com.winlator.star.core.NativeAudio.init();
            playing = true;
            // Si inicializa con éxito, saltamos la creación del stream clásico de ALSA
            return;
        } catch (Throwable e) {
            // Fallback: si no encuentra la librería nativa del wrapper, continúa por el path original
        }

        if (!isValidBufferSize()) return;

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

        // 🔊 DESVÍO ATÓMICO NATIVEAUDIO PARA CHIPS MALI:
        // Si estamos reproduciendo pero no hay stream de ALSA tradicional (streamPtr == 0),
        // transformamos el ByteBuffer directo de Wine en shorts y lo enviamos al wrapper de C.
        if (playing && streamPtr == 0) {
            try {
                int shortCount = data.remaining() / 2;
                if (shortCount > 0) {
                    short[] samples = new short[shortCount];
                    data.asShortBuffer().get(samples);
                    com.winlator.star.core.NativeAudio.write(samples, shortCount);
                    position += (shortCount / channelCount);
                }
                data.position(data.limit());
                return;
            } catch (Throwable e) {
                // Protección de desbordamientos en la conversión de buffer
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
