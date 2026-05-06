package com.example.android;

import android.net.Uri;
import android.os.Bundle;

import androidx.annotation.Nullable;
import androidx.appcompat.app.AppCompatActivity;
import androidx.media3.common.MediaItem;
import androidx.media3.exoplayer.ExoPlayer;

public class MainActivity extends AppCompatActivity {

    private ExoPlayer player;

    @Override
    protected void onCreate(@Nullable Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);

        player = new ExoPlayer.Builder(this).build();

        String streamUrl = "udp://10.0.2.2:3000";

        MediaItem mediaItem = MediaItem.fromUri(
                Uri.parse(streamUrl)
        );

        player.setMediaItem(mediaItem);
        player.prepare();
        player.play();
    }

    @Override
    protected void onDestroy() {
        if (player != null) {
            player.release();
        }
        super.onDestroy();
    }
}