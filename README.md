<div align="center">
  <h3 align="center">synth-c</h3>
  <p align="center">
      Real time audio synthesis with a physical Arduino controller, a Raspberry Pi audio engine, and Android audio streaming.
  </p>
</div>

## About

This repository contains an implementation of an audio synthesizer that is wired as a distribured system of three components:

- [x] an Arduino controller that reads physical inputs;
- [x] an audio engine written in C using `miniaudio` that runs in a Raspberry Pi;
- [x] an Android app that receives and plays the audio stream while also displays the waveform through an oscilloscope.

## Features

- Multiple waveform selection
- Polyphonic note triggering
- ADSR envelope control
- LFO modulation
- Pitch deviation
- Low-pass and high-pass filtering
- Dedicated physical controller
- UDP audio streaming to Android
- Android playback and oscilloscope visualization

## Arduino Controller

Overall, the arduino polls the physical controls, compacts them and sends them as a data packet through the serial port on a loop. It does some minor normalization and maintains state for tackling interference and mainly displaying the selected audioform through the lcd. The packet contains all the inputs and a checksum. Implementation for this segment is in c. If you want to change the wiring or add more inputs mess with [conf.ino](arduino/conf/conf.ino).

### Hardware

To build the controller you need the following components:

- Arduino (Mega)
- Ultrasonic proximity sensor
- Joystick
- Five potentiometers
- Six push buttons
- LCD screen
- Breadboard cables and resistors

### Usage

Plug it in through USB to your rasp or your computer if you are running the audio engine there to use the controller.
The energy it get's through the cable should suffice for it all to run seemlessly.

Then, as it is wired by default you should get the following behaviour.
The buttons are used to trigger chords on press and stop them on release. There is one that sits next to the lcd that toggles the waveform.
Four potentiometers control the wave envelope (attack, decay, sustain and release). One controls the LFO depth.
The ultrassonic sensor controls the LFO frequency. This one is the most fun, if you don't hear it bump up the LFO depth.
The joystick controls pitch deviation and filtering. This is close to what some MIDI keyboards do.
The lcd displays the selected waveform.

## Audio Engine

For the purposes of this system the Raspberry Pi is responsible for generating the audio, but you can also run this on your computer just fine.

The audio engine is written in C using `miniaudio`. It receives control packets from the controller and updates the synthesizer state in real time while the audio is running.

The engine supports:

- sine wave;
- square wave;
- triangle wave;
- sawtooth wave;
- ADSR envelope;
- LFO modulation;
- pitch deviation;
- low-pass filtering;
- high-pass filtering;
- polyphonic note playback.

The audio engine uses a node-based processing chain. Active notes are generated, shaped by an ADSR envelope, modulated by the LFO, filtered, and then written to the output buffer.

A secondary buffer is used to send the generated PCM frames to the Android app through UDP.

The audio engine is highly concurrent but lock free. It uses atomic variables and ringbuffers to handle the concurrent accesses. Having said that, it reads the data packets received on the serial port and normalizes each parameter. Then depending on the wiring chosen through the `update()` function it maps the normalized parameters to transformation. `update()` is also responsible for choosing the notes that each button plays. Feel free to interchange and play with configuration on [engine.c](rasp/engine.c)
A transformation to the audio is represented as an `Event`. At each iteration of the `dataCallback()` function every event in the event ring buffer is popped and applied to the global state which is then used to reinitialize the nodes in the node graph (source wave, filters and modulation). After that the new frames are generated, played and commited on the secondary buffer. They are then sent to the Android app through UDP.
This arquitecture alied with the serial port communication with the controller allows for very low latency end-to-end, something critical for the transformations applied to feel immediate, the same way it feels to play a musical instrument.

### Android App

The Android app receives the audio stream from the Raspberry Pi.

It sends an initial UDP handshake to the Raspberry Pi and then starts receiving audio packets. The received samples are played using `AudioTrack`.

The app also displays the received waveform in real time using an oscilloscope-style visualizer.

This is the simpler part of the application and is not at all necessary. The audio engine alied with the controller works wonders, but this app allowed us to stress test how low could we get the latency for this "embeded system". After all this was done for Embeded Systems class.

## Running

For the controller make sure to compile and export the [program](arduino/conf/conf.ino) through the arduino IDE.

Compile and run the audio engine with:

```bash
$ make run
```

The Raspberry Pi expects the Arduino to be connected through serial communication.

You should be able to play the synthesizer just by connecting headphones to the Raspberry Pi.

The android app and UDP communication is wonky so run the synthesizer before spinning up the android app.
