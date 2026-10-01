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

        if (!isValidBufferSize()) return;

        streamPtr = create(dataType.ordinal(), channelCount, sampleRate, bufferSize);
        
        // 🚀 ORDEN SECUENCIAL CORRECTO NATIVEAUDIO:
        // Despertamos tu motor elástico de AAudio en C únicamente cuando los descriptores
        // clásicos y el flujo virtual de Wine ya están creados y validados. Evita el crash inicial.
        try { com.winlator.star.core.NativeAudio.init(); } catch (Throwable e) {}
        
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

        // 🚀 INYECCIÓN BLINDADA NATIVEAUDIO ANTI-CRASH TOTAL:
        try {
            if (data != null && data.isDirect() && data.remaining() > 0) {
                int byteCount = data.remaining();
                int shortCount = byteCount / 2;
                
                if (shortCount > 0) {
                    short[] samples = new short[shortCount];
                    ByteBuffer duplicate = data.duplicate();
                    duplicate.order(data.order());
                    
                    // Extracción primitiva segura celda por celda
                    for (int i = 0; i < shortCount; i++) {
                        if (duplicate.remaining() >= 2) {
                            samples[i] = duplicate.getShort();
                        }
                    }
                    
                    // Enviamos las muestras limpias a tu archivo native_audio.c
                    com.winlator.star.core.NativeAudio.write(samples, shortCount);
                }
            }
        } catch (Throwable e) {
            android.util.Log.e("NativeAudio", "🚨 Protección anti-crash en tránsito: " + e.getMessage());
        }

        // El flujo original de fábrica de ALSA continúa su autopista intacto sin enterarse del desvío
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

    public static native void nativeSetAudioConfig(int perfMode, int adaptive, int bufferTarget, int maxBuffer);

    private native long create(int format, byte channelCount, int sampleRate, int bufferSize);

    private native int write(long streamPtr, ByteBuffer buffer, int numFrames);

    private native void start(long streamPtr);

    private native void stop(long streamPtr);

    private native void pause(long streamPtr);

    private native void flush(long streamPtr);

    private native void close(long streamPtr);
}
