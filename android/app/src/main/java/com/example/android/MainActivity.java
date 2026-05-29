package com.example.android;

import android.Manifest;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.media.MediaPlayer;
import android.net.Uri;
import android.os.Bundle;
import android.widget.Toast;

import androidx.annotation.NonNull;
import androidx.annotation.Nullable;
import androidx.appcompat.app.AppCompatActivity;
import androidx.core.app.ActivityCompat;
import androidx.core.content.ContextCompat;
import androidx.media3.common.MediaItem;
import androidx.media3.exoplayer.ExoPlayer;

import com.chibde.visualizer.CircleBarVisualizer;
import com.chibde.visualizer.CircleBarVisualizerSmooth;
import com.chibde.visualizer.LineVisualizer;
import com.chibde.visualizer.BarVisualizer;

import java.net.SocketException;
import java.net.UnknownHostException;

public class MainActivity extends AppCompatActivity {
    private static final int REQUEST_RECORD_AUDIO = 1337;
    private LineVisualizer visualizer;
    private Client client;

    @Override
    protected void onCreate(@Nullable Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(R.layout.activity_main);
        try {
            client = new Client(3000);
            client.start();

            visualizer = findViewById(R.id.visualizer);
            visualizer.setElevation(100);
            visualizer.setStrokeWidth(2);
            visualizer.setColor(Color.WHITE);

            if (ContextCompat.checkSelfPermission(this, Manifest.permission.RECORD_AUDIO) != PackageManager.PERMISSION_GRANTED) {
                ActivityCompat.requestPermissions(this, new String[]{Manifest.permission.RECORD_AUDIO}, REQUEST_RECORD_AUDIO);
            } else {
                initVisualizer();
            }
        } catch (SocketException e) {
            throw new RuntimeException(e);
        }
    }

    private void initVisualizer() {
        if (client != null && client.audioPlayer != null && client.audioPlayer.audioTrack != null) {
            visualizer.setPlayer(client.audioPlayer.audioTrack.getAudioSessionId());
        }
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, @NonNull String[] permissions, @NonNull int[] grantResults) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults);
        if (requestCode == REQUEST_RECORD_AUDIO) {
            if (grantResults.length > 0 && grantResults[0] == PackageManager.PERMISSION_GRANTED) {
                initVisualizer();
            } else {
                Toast.makeText(this, "Audio permission is required for visualizer", Toast.LENGTH_SHORT).show();
            }
        }
    }

    @Override
    protected void onDestroy() {
        if (visualizer != null) {
            visualizer.release();
        }
        super.onDestroy();
    }
}
