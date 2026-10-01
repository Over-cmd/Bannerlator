package com.winlator.star.alsaserver;

import com.winlator.star.sysvshm.SysVSharedMemory;
import com.winlator.star.xconnector.Client;
import com.winlator.star.xconnector.RequestHandler;
import com.winlator.star.xconnector.XConnectorEpoll;
import com.winlator.star.xconnector.XInputStream;
import com.winlator.star.xconnector.XOutputStream;
import com.winlator.star.xconnector.XStreamLock;

import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;

public class ALSARequestHandler implements RequestHandler {
    private int maxSHMemoryId = 0;
    // 🚀 CONTADOR DE POSICIÓN VIRTUAL NATIVEAUDIO:
    // Mantiene el registro global de frames procesados para simular el avance del reloj ante Wine
    private int virtualAudioPosition = 0;

    @Override
    public boolean handleRequest(Client client) throws IOException {
        ALSAClient alsaClient = (ALSAClient)client.getTag();
        XInputStream inputStream = client.getInputStream();
        XOutputStream outputStream = client.getOutputStream();

        if (inputStream.available() < 5) return false;
        byte requestCode = inputStream.readByte();
        int requestLength = inputStream.readInt();

        // 🚀 INDEPENDENCIA REAL MULTI-DRIVER:
        boolean useNativeAudio = true; 

        switch (requestCode) {
            case RequestCodes.CLOSE:
                if (useNativeAudio) {
                    try { com.winlator.star.core.NativeAudio.terminate(); } catch (Throwable e) {}
                } else {
                    if (alsaClient != null) alsaClient.release();
                }
                break;
            case RequestCodes.START:
                if (!useNativeAudio && alsaClient != null) alsaClient.start();
                break;
            case RequestCodes.STOP:
                if (!useNativeAudio && alsaClient != null) alsaClient.stop();
                break;
            case RequestCodes.PAUSE:
                if (!useNativeAudio && alsaClient != null) alsaClient.pause();
                break;
            case RequestCodes.PREPARE:
                if (inputStream.available() < requestLength) return false;

                byte channels = inputStream.readByte();
                byte dataTypeOrdinal = inputStream.readByte();
                int sampleRate = inputStream.readInt();
                int bufferSize = inputStream.readInt();

                // 🏗️ CONFIGURACIÓN SANEADA NATIVEAUDIO (SIN EXTRACCIÓN TRADICIONAL):
                // Seteamos las variables en Java para que el objeto no quede huérfano,
                // pero NO llamamos a alsaClient.prepare() para no secuestrar los códecs.
                if (alsaClient != null) {
                    alsaClient.setChannelCount(channels);
                    alsaClient.setDataType(ALSAClient.DataType.values()[dataTypeOrdinal]);
                    alsaClient.setSampleRate(sampleRate);
                    alsaClient.setBufferSize(bufferSize);
                }
                virtualAudioPosition = 0;

                // Inicializamos tu native_audio.c aislado en C
                try { com.winlator.star.core.NativeAudio.init(); } catch (Throwable e) {}
                
                // Mapeamos el entorno elástico de memoria compartida virtual reglamentario
                int size = bufferSize * (channels * 2);
                int fd = SysVSharedMemory.createMemoryFd("alsa-shm"+(++maxSHMemoryId), size);
                if (fd >= 0 && alsaClient != null) {
                    ByteBuffer buffer = SysVSharedMemory.mapSHMSegment(fd, size, 0, true);
                    if (buffer != null) alsaClient.setSharedBuffer(buffer);
                }

                try (XStreamLock lock = outputStream.lock()) {
                    outputStream.writeByte((byte)0);
                    outputStream.setAncillaryFd(fd);
                }
                break;
            case RequestCodes.WRITE:
                if (useNativeAudio) {
                    // 🚀 BOMBEO ELÁSTICO DE ENTRADA ANTI-COLISIÓN:
                    int timeout = 0;
                    while (inputStream.available() < requestLength && timeout < 100) {
                        try { Thread.sleep(1); } catch (InterruptedException e) {}
                        timeout++;
                    }

                    if (inputStream.available() < requestLength) return false;
                    ByteBuffer rawBuffer = inputStream.readByteBuffer(requestLength);
                    
                    // 🔊 INYECCIÓN DIRECTA AL SILICIO DE C:
                    try {
                        rawBuffer.order(ByteOrder.LITTLE_ENDIAN);
                        int shortCount = rawBuffer.remaining() / 2;
                        if (shortCount > 0) {
                            short[] samples = new short[shortCount];
                            rawBuffer.asShortBuffer().get(samples);
                            com.winlator.star.core.NativeAudio.write(samples, shortCount);
                            
                            // 🚀 SIMULACIÓN DE AVANCE DEL RELOJ:
                            // Incrementamos la posición en función de los frames consumidos (Muestras / Canales)
                            int channelsCount = (alsaClient != null) ? alsaClient.getChannelCount() : 2;
                            virtualAudioPosition += (shortCount / channelsCount);
                        }
                    } catch (Throwable e) {}

                    // 🚀 RESPUESTA DE CONTROL OBLIGATORIA (ACK):
                    try (XStreamLock lock = outputStream.lock()) {
                        outputStream.writeByte((byte)0);
                    }
                } else {
                    if (alsaClient != null) {
                        ByteBuffer buffer = alsaClient.getSharedBuffer();
                        if (buffer != null) {
                            buffer.limit(requestLength);
                            alsaClient.writeDataToStream(buffer);
                        } else {
                            if (inputStream.available() < requestLength) return false;
                            alsaClient.writeDataToStream(inputStream.readByteBuffer(requestLength));
                        }
                    }
                }
                break;
            case RequestCodes.DRAIN:
                if (!useNativeAudio && alsaClient != null) alsaClient.drain();
                break;
            case RequestCodes.POINTER:
                try (XStreamLock lock = outputStream.lock()) {
                    // 🔊 ENGAÑO PERFECTO A WINE:
                    // Devolvemos el puntero virtual incremental para simular el hardware corriendo libre.
                    // Esto remueve el congelamiento por completo y mantiene la sincronización.
                    outputStream.writeInt(useNativeAudio ? virtualAudioPosition : (alsaClient != null ? alsaClient.pointer() : 0));
                }
                break;
        }
        return true;
    }

    private void createSharedMemory(ALSAClient alsaClient, XOutputStream outputStream) throws IOException {
        int size = alsaClient.getBufferSizeInBytes();
        int fd = SysVSharedMemory.createMemoryFd("alsa-shm"+(++maxSHMemoryId), size);

        if (fd >= 0) {
            ByteBuffer buffer = SysVSharedMemory.mapSHMSegment(fd, size, 0, true);
            if (buffer != null) alsaClient.setSharedBuffer(buffer);
        }

        try (XStreamLock lock = outputStream.lock()) {
            outputStream.writeByte((byte)0);
            outputStream.setAncillaryFd(fd);
        }
        finally {
            if (fd >= 0) XConnectorEpoll.closeFd(fd);
        }
    }
}
