package com.example.android;

import android.os.Bundle;
import android.view.View;
import android.widget.Button;
import android.widget.EditText;

import androidx.activity.EdgeToEdge;
import androidx.appcompat.app.AppCompatActivity;
import androidx.core.graphics.Insets;
import androidx.core.view.ViewCompat;
import androidx.core.view.WindowInsetsCompat;

import java.io.IOException;
import java.net.SocketException;
import java.net.UnknownHostException;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.Scanner;

public class MainActivity extends AppCompatActivity {

    EditText textInput;
    Button sendButton;

    Client client;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);

        textInput = (EditText) findViewById(R.id.inputText);
        sendButton = (Button) findViewById(R.id.sendButton);

        try {
            client = new Client(4000);
        } catch (SocketException | UnknownHostException e) {
            throw new RuntimeException(e);
        }
    }

    public void sendReceiveMessage(View view) {
        new Thread (() -> {
            String input = textInput.getText().toString();
            client.buffer = input.getBytes(StandardCharsets.UTF_8);

            try {
                client.sendPacket();
            } catch (IOException e) {
                throw new RuntimeException(e);
            }

            byte[] packet = null;
            try {
            packet = client.receivePacket();
            } catch (IOException e) {
                throw new RuntimeException(e);
            }
            System.out.println("Received: " + Arrays.toString(packet));
        }).start();
    }
}