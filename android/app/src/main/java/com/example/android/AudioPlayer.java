package com.example.android;

import android.content.Context;
import android.media.AudioFormat;
import android.media.AudioManager;
import android.media.AudioTrack;

public class AudioPlayer {
    public AudioTrack audioTrack;

    AudioPlayer() {
        int sampleRate = 48000;

        int bufsize = AudioTrack.getMinBufferSize(
                sampleRate,
                AudioFormat.CHANNEL_OUT_STEREO,
                AudioFormat.ENCODING_PCM_FLOAT
        );

        audioTrack = new AudioTrack(
                AudioManager.STREAM_MUSIC,
                sampleRate,
                AudioFormat.CHANNEL_OUT_STEREO,
                AudioFormat.ENCODING_PCM_FLOAT,
                bufsize,
                AudioTrack.MODE_STREAM
        );

        audioTrack.setVolume(AudioTrack.getMaxVolume());

        audioTrack.play();
    }
}