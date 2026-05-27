package com.example.android;

import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioTrack;
import android.util.Log;

import java.io.IOException;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.net.SocketException;
import java.net.UnknownHostException;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;

public class Client extends Thread {
    private static final int SAMPLE_RATE = 48000;
    private static final int BUFFER_BYTES = 4096;

    private final DatagramSocket socket;
    private final byte[] buffer = new byte[BUFFER_BYTES];
    private final int serverPort;

    public Client(int serverPort) throws SocketException {
        socket = new DatagramSocket();
        this.serverPort = serverPort;
    }

    @Override
    public void run() {
        Log.d("CLIENT", "thread started");

        int minBuf = AudioTrack.getMinBufferSize(
                SAMPLE_RATE,
                AudioFormat.CHANNEL_OUT_STEREO,
                AudioFormat.ENCODING_PCM_FLOAT
        );

        AudioTrack audioTrack = new AudioTrack(
                AudioManager.STREAM_MUSIC,
                SAMPLE_RATE,
                AudioFormat.CHANNEL_OUT_STEREO,
                AudioFormat.ENCODING_PCM_FLOAT,
                Math.max(minBuf, BUFFER_BYTES),
                AudioTrack.MODE_STREAM
        );
        audioTrack.setVolume(AudioTrack.getMaxVolume());
        audioTrack.play();

        try {
            byte[] sendData = "hi".getBytes();
            DatagramPacket sendPacket = new DatagramPacket(
                    sendData, sendData.length,
                    InetAddress.getByName("10.0.2.2"), serverPort
            );
            socket.send(sendPacket);
        } catch (IOException e) {
            Log.e("CLIENT", "handshake failed", e);
            return;
        }

        DatagramPacket packet = new DatagramPacket(buffer, buffer.length);

        for (;;) {
            try {
                socket.receive(packet);

                int bytesReceived = packet.getLength();
                
                int floatCount = bytesReceived / 4;
                float[] floatBuffer = new float[floatCount];

                ByteBuffer.wrap(packet.getData(), 0, bytesReceived)
                        .order(ByteOrder.LITTLE_ENDIAN)
                        .asFloatBuffer()
                        .get(floatBuffer);

                audioTrack.write(floatBuffer, 0, floatCount, AudioTrack.WRITE_BLOCKING);

                Log.d("CLIENT", "received " + bytesReceived + " bytes / " + floatCount + " floats");

            } catch (IOException e) {
                Log.e("CLIENT", "receive failed", e);
            }
        }
    }
}