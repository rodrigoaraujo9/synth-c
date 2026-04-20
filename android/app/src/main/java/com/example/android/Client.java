package com.example.android;

import java.io.IOException;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.net.SocketException;
import java.net.UnknownHostException;
import java.util.Arrays;

public class Client extends Thread {
    private DatagramSocket socket;
    private final InetAddress address;
    private final int serverPort;

    private boolean running;
    private byte[] buffer = new byte[1024];

    public Client(int serverPort) throws SocketException, UnknownHostException {
        socket = new DatagramSocket();
        address = InetAddress.getByName("localhost");
        this.serverPort = serverPort;
    }

    public byte[] receivePacket() throws IOException {
        DatagramPacket packet = new DatagramPacket(buffer, buffer.length, address, serverPort);
        socket.receive(packet);
        return packet.getData();
    }

    public void run() {
        while (running) {
            byte[] packet = null;
            try {
                packet = receivePacket();
            } catch (IOException e) {
                throw new RuntimeException(e);
            }
            System.out.println("Packet: " + Arrays.toString(packet));
        }
    }

    public void close() {
        socket.close();
    }
}
