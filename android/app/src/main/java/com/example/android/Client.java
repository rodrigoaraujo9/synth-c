package com.example.android;

import java.io.IOException;
import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.net.SocketException;
import java.net.UnknownHostException;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.Scanner;

public class Client {
    private DatagramSocket socket;
    private final InetAddress address;
    private final int serverPort;

    private boolean running;
    public byte[] buffer = new byte[1024];

    public Client(int serverPort) throws SocketException, UnknownHostException {
        socket = new DatagramSocket();
        address = InetAddress.getByName("10.0.2.2");
        this.serverPort = serverPort;
    }

    public void sendPacket() throws IOException {
        DatagramPacket packet = new DatagramPacket(buffer, buffer.length, address, serverPort);
        socket.send(packet);
    }

    public byte[] receivePacket() throws IOException {
        DatagramPacket packet = new DatagramPacket(buffer, buffer.length, address, serverPort);
        socket.receive(packet);
        return packet.getData();
    }

    public void run() {
        Scanner scanner = new Scanner(System.in);

        while (running) {
            System.out.print("Write the message you want to send: ");
            String input = scanner.nextLine();
            buffer = input.getBytes(StandardCharsets.UTF_8);

            try {
                sendPacket();
            } catch (IOException e) {
                throw new RuntimeException(e);
            }

            byte[] packet = null;
            try {
                packet = receivePacket();
            } catch (IOException e) {
                throw new RuntimeException(e);
            }
            System.out.println("Received: " + Arrays.toString(packet));
        }
    }

    public void close() {
        socket.close();
    }

    public int main() throws SocketException, UnknownHostException {
        Client c = new Client(4000);
        c.run();

        return 0;
    }
}
