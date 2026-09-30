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

    @Override
    public boolean handleRequest(Client client) throws IOException {
        ALSAClient alsaClient = (ALSAClient)client.getTag();
        XInputStream inputStream = client.getInputStream();
        XOutputStream outputStream = client.getOutputStream();

        if (inputStream.available() < 5) return false;
        byte requestCode = inputStream.readByte();
        int requestLength = inputStream.readInt();

        // 🚀 DETECTOR DEL ENTRAMADO INDEPENDIENTE:
        // Si alsaClient no está inicializado o el buffer es cero, gobernamos bajo "nativeaudio".
        boolean useNativeAudio = (alsaClient == null || alsaClient.getBufferSize() == 0);

        switch (requestCode) {
            case RequestCodes.CLOSE:
                if (useNativeAudio) {
                    try { com.winlator.star.core.NativeAudio.terminate(); } catch (Throwable e) {}
                } else {
                    alsaClient.release();
                }
                break;
            case RequestCodes.START:
                if (!useNativeAudio) alsaClient.start();
                break;
            case RequestCodes.STOP:
                if (!useNativeAudio) alsaClient.stop();
                break;
            case RequestCodes.PAUSE:
                if (!useNativeAudio) alsaClient.pause();
                break;
            case RequestCodes.PREPARE:
                if (inputStream.available() < requestLength) return false;

                byte channels = inputStream.readByte();
                byte dataTypeOrdinal = inputStream.readByte();
                int sampleRate = inputStream.readInt();
                int bufferSize = inputStream.readInt();

                if (alsaClient != null) {
                    alsaClient.setChannelCount(channels);
                    alsaClient.setDataType(ALSAClient.DataType.values()[dataTypeOrdinal]);
                    alsaClient.setSampleRate(sampleRate);
                    alsaClient.setBufferSize(bufferSize);
                    alsaClient.prepare();
                    createSharedMemory(alsaClient, outputStream);
                } else {
                    // 🏗️ INICIALIZACIÓN MUTEADA DEL PROPIO MOTOR:
                    try { com.winlator.star.core.NativeAudio.init(); } catch (Throwable e) {}
                    
                    int size = bufferSize * (channels * 2);
                    int fd = SysVSharedMemory.createMemoryFd("alsa-shm"+(++maxSHMemoryId), size);
                    try (XStreamLock lock = outputStream.lock()) {
                        outputStream.writeByte((byte)0);
                        outputStream.setAncillaryFd(fd);
                    } finally {
                        if (fd >= 0) XConnectorEpoll.closeFd(fd);
                    }
                }
                break;
            case RequestCodes.WRITE:
                if (useNativeAudio) {
                    // 🚀 BOMBEO DE RED SEGURO NATIVEAUDIO:
                    // Forzamos un bucle elástico de espera en el socket local para que los trozos 
                    // de audio fragmentados de Wine se junten completos en la RAM antes de leer.
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
                        }
                    } catch (Throwable e) {}
                } else {
                    ByteBuffer buffer = alsaClient.getSharedBuffer();
                    if (buffer != null) {
                        buffer.limit(requestLength);
                        alsaClient.writeDataToStream(buffer);
                    } else {
                        if (inputStream.available() < requestLength) return false;
                        alsaClient.writeDataToStream(inputStream.readByteBuffer(requestLength));
                    }
                }
                break;
            case RequestCodes.DRAIN:
                if (!useNativeAudio) alsaClient.drain();
                break;
            case RequestCodes.POINTER:
                try (XStreamLock lock = outputStream.lock()) {
                    outputStream.writeInt(useNativeAudio ? 0 : alsaClient.pointer());
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
