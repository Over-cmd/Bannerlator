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

        switch (requestCode) {
            case RequestCodes.CLOSE:
                if (alsaClient != null) alsaClient.release();
                break;
            case RequestCodes.START:
                if (alsaClient != null) alsaClient.start();
                break;
            case RequestCodes.STOP:
                if (alsaClient != null) alsaClient.stop();
                break;
            case RequestCodes.PAUSE:
                if (alsaClient != null) alsaClient.pause();
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
                }
                break;
            case RequestCodes.WRITE:
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
                break;
            case RequestCodes.DRAIN:
                if (alsaClient != null) alsaClient.drain();
                break;
            case RequestCodes.POINTER:
                try (XStreamLock lock = outputStream.lock()) {
                    outputStream.writeInt(alsaClient != null ? alsaClient.pointer() : 0);
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
