//! Base for this implementation was:
//! https://miniaud.io/docs/examples/node_graph.html

#include <stdlib.h>
#define MINIAUDIO_IMPLEMENTATION
#include "lib/miniaudio.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
    void main_loop__em(void) {}
#endif


/* ------------------------------------------------------------------------------------------------------------- */

/* Config */

// Comm
#define PORT 3000
#define GROUP "239.0.0.1"

// Packet
#define PACKET_START 0xAA
#define PACKET_END 0x55

// Data Format
#define FORMAT ma_format_f32
#define CHANNELS 2
#define SAMPLE_RATE 48000
#define BUFFER_SIZE 2048

// Effect Properties
#define LPF_ORDER 8 // how agressive freqs beyond cuttof are attenuated (8 is very agressive)
#define HPF_ORDER 8

// Wave Config
#define BASE_AMP 0.2f
#define BASE_FREQ 440.0f // A4
#define BASE_NOTE 69     // A4
#define BASE_TYPE ma_waveform_type_sawtooth

// Input Bounds
#define NOTE_MAX 128
#define MAX_NOTES NOTE_MAX // like keyboard. one key per note

#define ATTACK_MAX 5.0f
#define ATTACK_MIN 0.0f

#define DECAY_MAX 1.0f
#define DECAY_MIN 0.0f

#define SUSTAIN_MAX 1.0f
#define SUSTAIN_MIN 0.0f

#define RELEASE_MAX 5.0f
#define RELEASE_MIN 0.0f

#define PITCH_OFFSET_MAX 2.0f
#define PITCH_OFFSET_MIN -2.0f

#define LFO_FREQUENCY_MAX 20.0f
#define LFO_FREQUENCY_MIN 0.0f

#define LFO_DEPTH_MAX 1.0f
#define LFO_DEPTH_MIN 0.0f

#define LPF_CUTOFF_MAX 18000.0f
#define LPF_CUTOFF_MIN 20.0f

#define HPF_CUTOFF_MAX 4000.0f
#define HPF_CUTOFF_MIN 20.0f

/* ------------------------------------------------------------------------------------------------------------- */

/* Types */

typedef struct {
    ma_atomic_uint32   waveform;        // [0..4] will be mapped to ma_waveform_type

    ma_atomic_float    attack;          // Seconds
    ma_atomic_float    decay;           // [0..1]
    ma_atomic_float    sustain;         // [0..1]
    ma_atomic_float    release;         // Seconds

    ma_atomic_uint32   keys[MAX_NOTES]; // [0..1] Active keys (mapped by note)

    ma_atomic_float    pitch_offset;    // Semi-tones makes more sense for logic but less for readability

    ma_atomic_float    lfo_frequency;   // Hz
    ma_atomic_float    lfo_depth;       // [0..1]

    ma_atomic_float    lpf_cutoff;      // Hz
    ma_atomic_float    hpf_cutoff;      // Hz
} State;

typedef enum {
    SET_WAVE,
    NOTE_PRESSED,
    NOTE_RELEASED,
    SET_PITCH_OFFSET,
    SET_LFO_FREQUENCY,
    SET_LFO_DEPTH,
    SET_LPF_CUTOFF,
    SET_HPF_CUTOFF,
    SET_ATTACK,
    SET_DECAY,
    SET_SUSTAIN,
    SET_RELEASE
} Type;

typedef struct {
    Type type;

    // for SET_WAVE directly represents wave selected
    // for NOTE_PRESSED represents MIDI value associated with given note played -> consult table
    // for NOTE_RELEASED also represents MIDI value, of the note we want to stop playing
    ma_uint32 value;
} Event;

typedef struct {
    ma_node_base base;
    ma_float phase;
    ma_float sample_rate;
} Lfo;

typedef enum {
    IDLE = 0,
    ATTACK,
    DECAY,
    SUSTAIN,
    RELEASE
} Stage;

typedef struct {
    Stage stage;
    ma_float gain;               // [0..1] represents the current envelope gain or level (as in intensity of sound)
    ma_float release_start_gain; // represents the level at which the envelope was when it entered release stage
} Envelope;

typedef struct __attribute__((packed)) {
  uint8_t start;
  uint8_t waveform;
  int16_t potentiometers[5]; // LFO depth | Attack | Decay | Sustain | Release
  int16_t joystick[2];       // x | y
  float ultrasonic;
  uint8_t buttons[5];
  uint8_t checksum;
  uint8_t end;
} Packet;

/* ------------------------------------------------------------------------------------------------------------- */

/* Globals */

static State g_state;

static Packet g_conf;

static ma_waveform g_waves[MAX_NOTES];
static uint8_t g_active_notes[MAX_NOTES] = {0};
static Envelope g_envelopes[MAX_NOTES];

static ma_node_graph g_nodeGraph;
static ma_lpf_node g_lpfNode;
static ma_hpf_node g_hpfNode;
static Lfo g_lfoNode;
static ma_node_base g_polyNode;

static ma_rb g_eventBuf;
static unsigned char g_eventBufStore[sizeof(Event) * 256];

static ma_pcm_rb g_udpBuf;

/* ------------------------------------------------------------------------------------------------------------- */

/* Helper Functions */

/* ------------------------------------------------------------------------------------------------------------- */

/* Ring Buffer */

int push_event(const Event *event) {
    void *buffer = NULL;
    size_t bytes_to_write = sizeof(*event);

    if (ma_rb_acquire_write(&g_eventBuf, &bytes_to_write, &buffer) != MA_SUCCESS) {
        return 0;
    }

    if (bytes_to_write < sizeof(*event)) {
        ma_rb_commit_write(&g_eventBuf, 0);
        return 0;
    }

    memcpy(buffer, event, sizeof(*event));

    return ma_rb_commit_write(&g_eventBuf, sizeof(*event)) == MA_SUCCESS;
}

int pop_event(Event *event) {
    void *buffer = NULL;
    size_t bytes_to_read = sizeof(*event);

    if (ma_rb_acquire_read(&g_eventBuf, &bytes_to_read, &buffer) != MA_SUCCESS) {
        return 0;
    }

    if (bytes_to_read < sizeof(*event)) {
        ma_rb_commit_read(&g_eventBuf, 0);
        return 0;
    }

    memcpy(event, buffer, sizeof(*event));

    return ma_rb_commit_read(&g_eventBuf, sizeof(*event)) == MA_SUCCESS;
}


/* ------------------------------------------------------------------------------------------------------------- */

/* Event */

ma_uint32 float_as_event_value(ma_float value) {
    ma_uint32 raw = 0;
    memcpy(&raw, &value, sizeof(value));
    return raw;
}

ma_int32 event_value_as_int32(ma_uint32 raw) {
    return (ma_int32)raw;
}

ma_float event_value_as_float(ma_uint32 raw) {
    ma_float value = 0.0f;
    memcpy(&value, &raw, sizeof(value));
    return value;
}

int event_value_as_waveform(ma_uint32 value, ma_waveform_type *waveform) {
    if (waveform == NULL) {
        return -1;
    }

    switch (value) {
        case 0:
            *waveform = ma_waveform_type_sine;
            return 0;
        case 1:
            *waveform = ma_waveform_type_square;
            return 0;
        case 2:
            *waveform = ma_waveform_type_triangle;
            return 0;
        case 3:
            *waveform = ma_waveform_type_sawtooth;
            return 0;
        default:
            return -1;
    }
}


/* ------------------------------------------------------------------------------------------------------------- */

/* Note */

/// Returns the frequency of a MIDI note [0..128]
ma_float frequency_from_midi_note(ma_float note) {
    // c[-1] = 0
    // a[4]  = 69
    // to freq consider base -> 440 (a[4])
    // do the math with distance from a[4]
    const ma_float diff = note - BASE_NOTE;
    const ma_float freq = BASE_FREQ * ma_powf(2.0f, diff/12.0f);

    return freq;
}

/// Plays a MIDI note [0..128]
void note_on(ma_uint32 note) {
    if (note >= MAX_NOTES) return;

    // atack should ramp from current value so it flows nicely with
    // release instead of creating a click
    g_envelopes[note].stage = ATTACK;
    ma_atomic_uint32_set(&g_state.keys[note], 1);
}

/// Stops a MIDI note [0..128] if active
void note_off(ma_uint32 note) {
    if (note >= MAX_NOTES) return;

    if (g_envelopes[note].stage != IDLE && g_envelopes[note].stage != RELEASE) {
        g_envelopes[note].release_start_gain = g_envelopes[note].gain;
        g_envelopes[note].stage = RELEASE;
    }
}


/* ------------------------------------------------------------------------------------------------------------- */

/* Envelope */

/// Advance a single envelope by one sample. Returns current gain [0..1].
/// Clears the key bit when release finishes.
static inline ma_float envelope_step(ma_uint32 note, ma_float a, ma_float d, ma_float s, ma_float r) {
    Envelope *env = &g_envelopes[note];
    const ma_float dt = 1.0f / (ma_float)SAMPLE_RATE;

    switch (env->stage) {
        case ATTACK: {
            if (a <= 0.0f) {
                env->gain = 1.0f;
                env->stage = DECAY;
            } else {
                env->gain += dt / a;
                if (env->gain >= 1.0f) {
                    env->gain = 1.0f;
                    env->stage = DECAY;
                }
            }
            break;
        }
        case DECAY: {
            if (d <= 0.0f) {
                env->gain = s;
                env->stage = SUSTAIN;
            } else {
                env->gain -= (1.0f - s) * (dt / d);
                if (env->gain <= s) {
                    env->gain = s;
                    env->stage = SUSTAIN;
                }
            }
            break;
        }
        case SUSTAIN: {
            env->gain = s;
            break;
        }
        case RELEASE: {
            if (r <= 0.0f) {
                env->gain = 0.0f;
                env->stage = IDLE;
                ma_atomic_uint32_set(&g_state.keys[note], 0);
            } else {
                env->gain -= env->release_start_gain * (dt / r);
                if (env->gain <= 0.0f) {
                    env->gain = 0.0f;
                    env->stage = IDLE;
                    ma_atomic_uint32_set(&g_state.keys[note], 0);
                }
            }
            break;
        }
        case IDLE:
        default:
            env->gain = 0.0f;
            break;
    }

    return env->gain;
}

/* ------------------------------------------------------------------------------------------------------------- */

/* Normalization */

/// Normalized potentiometer output to a value between [0..1].
/// Returns 1 upon succes and 0 upon failiure.
int normalize_potentiometer(int in, ma_float *out) {
    const int max = 1023;
    const int min = 0;

    if (out == NULL) return 0;
    if (in < min || in > max) return 0;

    const ma_float range = (ma_float)(max - min);

    *out = ((ma_float)(in - min)) / range;

    return 1;
}

/// Normalized joystick output to a value between [0..1].
/// Returns 1 upon succes and 0 upon failiure.
int normalize_joystick(int in_x, int in_y, ma_float *out_x, ma_float *out_y) {
    const int max = 1023;
    const int min = 0;

    if (out_x == NULL || out_y == NULL) return 0;
    if (in_x < min || in_x > max) return 0;
    if (in_y < min || in_y > max) return 0;

    const ma_float range = (ma_float)(max - min);

    *out_x = (ma_float)(in_x - min) / range;
    *out_y = (ma_float)(in_y - min) / range;

    return 1;
}

/// Normalized ultrasonic output to a value between [0..1].
/// Returns 1 upon succes and 0 upon failiure.
int normalize_ultrasonic(ma_float in, ma_float *out)
{
    const ma_float min = 2.0f;
    const ma_float max = 40.0f;

    if (out == NULL) return 0;
    if (in < min || in > max) return 0;

    *out = 1.0f - ((in - min) / (max - min));

    return 1;
}


/* ------------------------------------------------------------------------------------------------------------- */

/* Update */

/// Updates waveform from a non-normalized input.
void update_waveform(uint8_t in) {
    Event event = {SET_WAVE, in};
    push_event(&event);
}

/// Updates LFO frequency from a normalized input [0..1].
void update_lfo_frequency(ma_float in) {
    ma_float value = LFO_FREQUENCY_MIN + in * (LFO_FREQUENCY_MAX - LFO_FREQUENCY_MIN);
    Event event = {SET_LFO_FREQUENCY, float_as_event_value(value)};
    push_event(&event);
}

/// Updates LFO depth from a normalized input [0..1].
void update_lfo_depth(ma_float in) {
    ma_float value = LFO_DEPTH_MIN + in * (LFO_DEPTH_MAX - LFO_DEPTH_MIN);
    Event event = {SET_LFO_DEPTH, float_as_event_value(value)};
    push_event(&event);
}

/// Updates HPF cutoff from a normalized input [0..1].
void update_hpf_cutoff(ma_float in) {
    ma_float value = HPF_CUTOFF_MIN + in * (HPF_CUTOFF_MAX - HPF_CUTOFF_MIN);
    Event event = {SET_HPF_CUTOFF, float_as_event_value(value)};
    push_event(&event);
}

/// Updates LPF cutoff from a normalized input [0..1].
void update_lpf_cutoff(ma_float in) {
    ma_float value = LPF_CUTOFF_MAX - in * (LPF_CUTOFF_MAX - LPF_CUTOFF_MIN);
    Event event = {SET_LPF_CUTOFF, float_as_event_value(value)};
    push_event(&event);
}

/// Updates pitch offset from a normalized input [0..1].
void update_pitch_offset(ma_float in) {
    ma_float value = PITCH_OFFSET_MIN + in * (PITCH_OFFSET_MAX - PITCH_OFFSET_MIN);
    Event event = {SET_PITCH_OFFSET, float_as_event_value(value)};
    push_event(&event);
}

/// Updates attack from a normalized input [0..1].
void update_attack(ma_float in) {
    ma_float value = ATTACK_MIN + in * (ATTACK_MAX - ATTACK_MIN);
    Event event = {SET_ATTACK, float_as_event_value(value)};
    push_event(&event);
}

/// Updates decay from a normalized input [0..1].
void update_decay(ma_float in) {
    ma_float value = DECAY_MIN + in * (DECAY_MAX - DECAY_MIN);
    Event event = {SET_DECAY, float_as_event_value(value)};
    push_event(&event);
}

/// Updates sustain from a normalized input [0..1].
void update_sustain(ma_float in) {
    ma_float value = SUSTAIN_MIN + in * (SUSTAIN_MAX - SUSTAIN_MIN);
    Event event = {SET_SUSTAIN, float_as_event_value(value)};
    push_event(&event);
}

/// Updates release from a normalized input [0..1].
void update_release(ma_float in) {
    ma_float value = RELEASE_MIN + in * (RELEASE_MAX - RELEASE_MIN);
    Event event = {SET_RELEASE, float_as_event_value(value)};
    push_event(&event);
}

/// Helper to make a note active safely.
void add_note(uint8_t notes[MAX_NOTES], ma_uint32 note) {
    if (note < MAX_NOTES) {
        notes[note] = 1;
    }
}

/// Updades envelopes to accomodate the current active notes.
/// Updades envelopes to accomodate the current active notes.
void update_envelopes() {
    uint8_t new_notes[MAX_NOTES] = {0};

    // /* Chords for Idioteque by Radiohead for demo */

    // // Button 0: Gm6/D = D - G - Bb - E
    // if (g_conf.buttons[0] != 0) {
    //     add_note(new_notes, 50); // D3
    //     add_note(new_notes, 55); // G3
    //     add_note(new_notes, 58); // Bb3
    //     add_note(new_notes, 64); // E4
    // }

    // // Button 1: Ebmaj9 = Eb - Bb - D - F - G - Bb - D
    // if (g_conf.buttons[1] != 0) {
    //     add_note(new_notes, 51); // Eb3
    //     add_note(new_notes, 58); // Bb3
    //     add_note(new_notes, 62); // D4
    //     add_note(new_notes, 65); // F4
    //     add_note(new_notes, 67); // G4
    //     add_note(new_notes, 70); // Bb4
    //     add_note(new_notes, 74); // D5
    // }

    // // Button 2: Gm = G - D - Bb - D
    // if (g_conf.buttons[2] != 0) {
    //     add_note(new_notes, 43); // G2
    //     add_note(new_notes, 50); // D3
    //     add_note(new_notes, 58); // Bb3
    //     add_note(new_notes, 62); // D4
    // }

    // // Button 3: Ebmaj9 = Eb - Bb - D - F - G
    // if (g_conf.buttons[3] != 0) {
    //     add_note(new_notes, 39); // Eb2
    //     add_note(new_notes, 46); // Bb2
    //     add_note(new_notes, 50); // D3
    //     add_note(new_notes, 53); // F3
    //     add_note(new_notes, 55); // G3
    // }

    // // Button 4: optional higher Gm6/D = D - G - Bb - E
    // if (g_conf.buttons[4] != 0) {
    //     add_note(new_notes, 62); // D4
    //     add_note(new_notes, 67); // G4
    //     add_note(new_notes, 70); // Bb4
    //     add_note(new_notes, 76); // E5
    // }



    /* Chords for Nangs by Tame Impala */

    /* Chords for Nangs by Tame Impala */

    // Button 0: Cmaj7
    if (g_conf.buttons[0] != 0) {
        add_note(new_notes, 60-12); // C
        add_note(new_notes, 64-12); // E
        add_note(new_notes, 67-12); // G
        add_note(new_notes, 71-12); // B
    }

    // Button 1: Dm7/G
    if (g_conf.buttons[1] != 0) {
        add_note(new_notes, 67-12); // G
        add_note(new_notes, 72-12); // C
        add_note(new_notes, 74-12); // D
        add_note(new_notes, 77-12); // F
        add_note(new_notes, 81-12); // A
    }

    // Button 2: Cmaj7
    if (g_conf.buttons[2] != 0) {
        add_note(new_notes, 60-12); // C
        add_note(new_notes, 64-12); // E
        add_note(new_notes, 67-12); // G
        add_note(new_notes, 71-12); // B
    }

    // Button 3: Ebmaj7
    if (g_conf.buttons[3] != 0) {
        add_note(new_notes, 63-12); // Eb
        add_note(new_notes, 67-12); // G
        add_note(new_notes, 70-12); // Bb
        add_note(new_notes, 74-12); // D
    }

    // Button 4: Abmaj9
    if (g_conf.buttons[4] != 0) {
        add_note(new_notes, 68-12); // Ab
        add_note(new_notes, 72-12); // C
        add_note(new_notes, 75-12); // Eb
        add_note(new_notes, 79-12); // G
        add_note(new_notes, 82-12); // Bb
    }

    for (ma_uint32 note = 0; note < MAX_NOTES; note++) {
        if (new_notes[note] && !g_active_notes[note]) {
            Event event = { NOTE_PRESSED, note };
            push_event(&event);
        }

        if (!new_notes[note] && g_active_notes[note]) {
            Event event = { NOTE_RELEASED, note };
            push_event(&event);
        }

        g_active_notes[note] = new_notes[note];
    }
}

/// Translates controller input into parameter updates.
void update() {
    ma_float attack, decay, sustain, release, frequency, distance, x, y;

    if (g_conf.waveform < 3 || g_conf.waveform > 0) {
        update_waveform(g_conf.waveform);
    }

    if (normalize_potentiometer(g_conf.potentiometers[0], &frequency)) {
        update_lfo_depth(frequency);
    }

    if (normalize_potentiometer(g_conf.potentiometers[1], &attack)) {
        update_attack(attack);
    }

    if (normalize_potentiometer(g_conf.potentiometers[2], &decay)) {
        update_decay(decay);
    }

    if (normalize_potentiometer(g_conf.potentiometers[3], &sustain)) {
        update_sustain(sustain);
    }

    if (normalize_potentiometer(g_conf.potentiometers[4], &release)) {
        update_release(release);
    }

    if (normalize_joystick(g_conf.joystick[0], g_conf.joystick[1], &x, &y)) {
        if (x <= 0.5) {
            update_lpf_cutoff((0.5f - x) * 2.0f);
            update_hpf_cutoff(0.0f);
        } else {
            update_hpf_cutoff((x - 0.5f) * 2.0f);
            update_lpf_cutoff(0.0f);
        }

        update_pitch_offset(y);
    }

    if (normalize_ultrasonic(g_conf.ultrasonic, &distance)) {
        update_lfo_frequency(distance);
    }

    update_envelopes();
}

/* ------------------------------------------------------------------------------------------------------------- */

/* Communication */

uint8_t checksum(Packet *p) {

  uint8_t *data = (uint8_t *)p;

  uint8_t sum = 0;

  // checksum and end byte are excluded
  for (size_t i = 0; i < sizeof(Packet) - 2; i++) {
    sum ^= data[i];
  }

  return sum;
}

int read_packet(int sfd, Packet *out) {
    uint8_t byte;

    for (;;) {
        if (read(sfd, &byte, 1) != 1) return 0;

        if (byte == PACKET_START) break;
    }

    uint8_t *bytes = (uint8_t *)out;
    bytes[0] = PACKET_START;

    size_t received = 1;

    while (received < sizeof(Packet)) {
        ssize_t n = read(sfd, bytes + received, sizeof(Packet) - received);

        if (n <= 0) return 0;

        received += n;
    }

    // Verify integrity of the packet

    if (out->end != PACKET_END || out->start != PACKET_START || out->checksum != checksum(out)) return 0;

    return 1;
}

void *poll_conf(void *arg) {
    #if defined(__APPLE__) && defined(__MACH__)
        #define SERIAL_PORT "/dev/cu.usbmodem1101"
    #elif defined(__linux__)
        #define SERIAL_PORT "/dev/ttyACM0"
    #else
        #error Unsupported platform
    #endif

    int sfd = open(SERIAL_PORT, O_RDWR | O_NOCTTY);
    if (sfd == -1) {
        printf("Error opening serial port: %s\n",strerror(errno));
        return NULL;
    }

    struct termios options;
    if (tcgetattr(sfd,&options) < 0) {
        printf("Error getting attributes: %s\n",strerror(errno));
        return NULL;
    }

    cfmakeraw(&options);
    cfsetspeed(&options, 115200);

    options.c_cflag &= ~CSTOPB;
    options.c_cflag |= CLOCAL;
    options.c_cflag |= CREAD;
    options.c_cflag &= ~CRTSCTS;
    options.c_cc[VTIME] = 0;
    options.c_cc[VMIN] = 1;

    if (tcsetattr(sfd,TCSANOW,&options) < 0) {
        printf("Error setting attributes: %s\n",strerror(errno));
        return NULL;
    }

    tcflush(sfd,TCIFLUSH);

    char c;

    for (;;) {
        Packet packet;

        if (!read_packet(sfd, &packet)) {
            continue;
        }

        g_conf = packet;

        printf("joystick: %d, %d, pot: %d, ultra: %f, adsr: %d, %d, %d, %d, buttons: %d, %d, %d, %d, %d\n", g_conf.joystick[0], g_conf.joystick[1], g_conf.potentiometers[0], g_conf.ultrasonic, g_conf.potentiometers[1], g_conf.potentiometers[2], g_conf.potentiometers[3], g_conf.potentiometers[4], g_conf.buttons[0], g_conf.buttons[1], g_conf.buttons[2], g_conf.buttons[3], g_conf.buttons[4]);


        update();
    }
}

void *send_audio_udp(void *arg) {
    int sockfd; /* socket */
    int clientlen; /* byte size of client's address */
    struct sockaddr_in serveraddr; /* server's addr */
    struct sockaddr_in clientaddr; /* client addr */
    struct hostent *hostp; /* client host info */
    char *hostaddrp; /* dotted decimal host addr string */
    int optval; /* flag value for setsockopt */
    int n; /* message byte size */
    char buf[1000]; /* connection request message byte size */

    /*
    * socket: create the parent socket
    */
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0)
      printf("ERROR opening socket");

    /* setsockopt: Handy debugging trick that lets
    * us rerun the server immediately after we kill it;
    * otherwise we have to wait about 20 secs.
    * Eliminates "ERROR on binding: Address already in use" error.
    */
    optval = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, (const void *)&optval , sizeof(int));

    /*
    * build the server's Internet address
    */
    bzero((char *) &serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
    serveraddr.sin_port = htons((unsigned short)PORT);

    /*
    * bind: associate the parent socket with a port
    */
    if (bind(sockfd, (struct sockaddr *) &serveraddr,
	   sizeof(serveraddr)) < 0)
      printf("ERROR on binding");

    /*
    * main loop: wait for a datagram, then echo it
    */
    clientlen = sizeof(clientaddr);
    while (1) {
        /*
        * recvfrom: receive a UDP datagram from a client
        */
        bzero(buf, 1000);
        n = recvfrom(sockfd, buf, 1000, 0,
            (struct sockaddr *) &clientaddr, &clientlen);
        if (n < 0) printf("ERROR in recvfrom");

        /*
        * gethostbyaddr: determine who sent the datagram
        */
        hostp = gethostbyaddr((const char *)&clientaddr.sin_addr.s_addr, sizeof(clientaddr.sin_addr.s_addr), AF_INET);
        if (hostp == NULL) printf("ERROR on gethostbyaddr");

        hostaddrp = inet_ntoa(clientaddr.sin_addr);
        if (hostaddrp == NULL) printf("ERROR on inet_ntoa\n");

        printf("server received datagram from %s (%s)\n", hostp->h_name, hostaddrp);
        printf("server received %d/%d bytes: %s\n", strlen(buf), n, buf);

        for (;;) {
            void* readBuffer;
            ma_uint32 frames_to_read = 5;

            if (ma_pcm_rb_acquire_read(&g_udpBuf, &frames_to_read, (void**)&readBuffer) == MA_SUCCESS) {
                size_t bytes = frames_to_read * CHANNELS * sizeof(float);

                if (frames_to_read > 0) {
                    sendto(sockfd, readBuffer, bytes, 0,
                        (struct sockaddr*)&clientaddr, clientlen);
                } else {
                    usleep(1000);
                }

                ma_pcm_rb_commit_read(&g_udpBuf, frames_to_read);
            }
        }
    }
}
/* ------------------------------------------------------------------------------------------------------------- */

/* Node Graph */

static void poly_node_process_pcm_frames(
    ma_node* pNode,
    const float** ppFramesIn,
    ma_uint32* pFrameCountIn,
    float** ppFramesOut,
    ma_uint32* pFrameCountOut
) {
    (void)pNode;
    (void)ppFramesIn;
    (void)pFrameCountIn;

    float* out = ppFramesOut[0];
    ma_uint32 framesToProcess = *pFrameCountOut;

    memset(out, 0, framesToProcess * CHANNELS * sizeof(float));

    const ma_waveform_type waveform = (ma_waveform_type)ma_atomic_uint32_get(&g_state.waveform);
    const ma_float offset = ma_atomic_float_get(&g_state.pitch_offset);

    const ma_float a = ma_atomic_float_get(&g_state.attack);
    const ma_float d = ma_atomic_float_get(&g_state.decay);
    const ma_float s = ma_atomic_float_get(&g_state.sustain);
    const ma_float r = ma_atomic_float_get(&g_state.release);

    ma_uint32 active_count = 0;
    for (ma_uint32 note = 0; note < MAX_NOTES; note++) {
        if (ma_atomic_uint32_get(&g_state.keys[note]) != 0) {
            active_count++;
        }
    }

    if (active_count == 0) {
        *pFrameCountOut = framesToProcess;
        return;
    }

    const ma_float amp = BASE_AMP / (ma_float)active_count;

    for (ma_uint32 note = 0; note < MAX_NOTES; note++) {
        if (ma_atomic_uint32_get(&g_state.keys[note]) == 0) {
            continue;
        }

        ma_waveform_set_type(&g_waves[note], waveform);
        ma_waveform_set_frequency(&g_waves[note], frequency_from_midi_note((ma_float)note + offset));
        ma_waveform_set_amplitude(&g_waves[note], amp);

        for (ma_uint32 i = 0; i < framesToProcess; i++) {
            float sample[CHANNELS];
            ma_waveform_read_pcm_frames(&g_waves[note], sample, 1, NULL);

            ma_float env_gain = envelope_step(note, a, d, s, r);

            for (ma_uint32 c = 0; c < CHANNELS; c++) {
                out[i * CHANNELS + c] += sample[c] * env_gain;
            }
        }
    }

    *pFrameCountOut = framesToProcess;
}

static ma_node_vtable g_polyNodeVTable = {
    poly_node_process_pcm_frames,
    NULL,
    0,
    1,
    0
};

static void lfo_node_process_pcm_frames(
    ma_node* pNode,
    const float** ppFramesIn,
    ma_uint32* pFrameCountIn,
    float** ppFramesOut,
    ma_uint32* pFrameCountOut
) {
    Lfo* lfo = (Lfo*)pNode;

    const float* in = ppFramesIn[0];
    float* out = ppFramesOut[0];

    ma_uint32 framesToProcess = *pFrameCountOut;

    ma_float lfo_frequency = ma_atomic_float_get(&g_state.lfo_frequency);
    ma_float lfo_depth = ma_atomic_float_get(&g_state.lfo_depth);

    for (ma_uint32 i = 0; i < framesToProcess; i++) {
        ma_float gain = (1.0f - lfo_depth) + (lfo_depth * ((1.0f + ma_sinf(2.0f * MA_PI * lfo->phase)) * 0.5f));
        for (ma_uint32 c = 0; c < CHANNELS; c++) {
            out[i * CHANNELS + c] = in[i * CHANNELS + c] * gain;
        }

        lfo->phase += lfo_frequency / lfo->sample_rate;

        while (lfo->phase >= 1.0f) {
            lfo->phase -= 1.0f;
        }
    }

    *pFrameCountIn = framesToProcess;
    *pFrameCountOut = framesToProcess;
}

static ma_node_vtable g_tremoloNodeVTable = {
    lfo_node_process_pcm_frames,
    NULL,
    1,
    1,
    0
};


/* ------------------------------------------------------------------------------------------------------------- */

/* Main Functions */

/* ------------------------------------------------------------------------------------------------------------- */

/* Callback */

// backend runs this funct on repeat
void data_callback(ma_device *pDevice, void *pOutput, const void *pInput, ma_uint32 frameCount) {
    Event event;

    MA_ASSERT(pDevice->playback.channels == CHANNELS);

    while (pop_event(&event)) {
        switch (event.type) {
            case SET_WAVE: {
                ma_waveform_type waveform;

                if (event_value_as_waveform(event.value, &waveform) != 0){
                    printf("*error* failed to convert uint32 to waveform");
                    break;
                }

                ma_atomic_uint32_set(&g_state.waveform, event.value);
                break;
            }

            case SET_ATTACK: {
                ma_float attack = event_value_as_float(event.value);

                if (attack > ATTACK_MAX || attack < ATTACK_MIN) {
                    printf("*error* out of bounds attack value on SET_ATTACK event");
                    break;
                }

                ma_atomic_float_set(&g_state.attack, attack);
                break;
            }

            case SET_DECAY: {
                ma_float decay = event_value_as_float(event.value);

                if (decay > DECAY_MAX || decay < DECAY_MIN) {
                    printf("*error* out of bounds decay value on SET_DECAY event");
                    break;
                }

                ma_atomic_float_set(&g_state.decay, decay);
                break;
            }

            case SET_SUSTAIN: {
                ma_float sustain = event_value_as_float(event.value);

                if (sustain > SUSTAIN_MAX || sustain < SUSTAIN_MIN) {
                    printf("*error* out of bounds sustain value on SET_SUSTAIN event");
                    break;
                }

                ma_atomic_float_set(&g_state.sustain, sustain);
                break;
            }

            case SET_RELEASE: {
                ma_float release = event_value_as_float(event.value);

                if (release > RELEASE_MAX || release < RELEASE_MIN) {
                    printf("*error* out of bounds release value on SET_RELEASE event");
                    break;
                }

                ma_atomic_float_set(&g_state.release, release);
                break;
            }

            case SET_PITCH_OFFSET: {
                ma_float offset = event_value_as_float(event.value);

                if (offset > PITCH_OFFSET_MAX || offset < PITCH_OFFSET_MIN) {
                    printf("*error* out of bounds pitch offset value on SET_PITCH_OFFSET event");
                    break;
                }

                ma_atomic_float_set(&g_state.pitch_offset, offset);
                break;
            }

            case SET_LFO_FREQUENCY: {
                ma_float lfo_freq = event_value_as_float(event.value);

                if (lfo_freq > LFO_FREQUENCY_MAX || lfo_freq < LFO_FREQUENCY_MIN) {
                    printf("*error* out of bounds LFO frequency value on SET_LFO_FREQUENCY event");
                    break;
                }

                ma_atomic_float_set(&g_state.lfo_frequency, lfo_freq);
                break;
            }

            case SET_LFO_DEPTH: {
                ma_float lfo_depth = event_value_as_float(event.value);

                if (lfo_depth > LFO_DEPTH_MAX || lfo_depth < LFO_DEPTH_MIN) {
                    printf("*error* out of bounds LFO depth value on SET_LFO_DEPTH event");
                    break;
                }

                ma_atomic_float_set(&g_state.lfo_depth, lfo_depth);
                break;
            }

            case SET_LPF_CUTOFF: {
                ma_float cutoff = event_value_as_float(event.value);

                if (cutoff > LPF_CUTOFF_MAX || cutoff < LPF_CUTOFF_MIN) {
                    printf("*error* out of bounds LPF cutoff value on SET_LPF_CUTOFF event");
                    break;
                }

                ma_atomic_float_set(&g_state.lpf_cutoff, cutoff);

                ma_lpf_config lpfConfig = ma_lpf_config_init(
                    FORMAT,
                    CHANNELS,
                    SAMPLE_RATE,
                    cutoff,
                    LPF_ORDER
                );

                ma_lpf_node_reinit(&lpfConfig, &g_lpfNode);

                break;
            }

            case SET_HPF_CUTOFF: {
                ma_float cutoff = event_value_as_float(event.value);

                if (cutoff > HPF_CUTOFF_MAX || cutoff < HPF_CUTOFF_MIN) {
                    printf("*error* out of bounds HPF cutoff value on SET_HPF_CUTOFF event");
                    break;
                }

                ma_atomic_float_set(&g_state.hpf_cutoff, cutoff);

                ma_hpf_config hpfConfig = ma_hpf_config_init(
                    FORMAT,
                    CHANNELS,
                    SAMPLE_RATE,
                    cutoff,
                    HPF_ORDER
                );

                ma_hpf_node_reinit(&hpfConfig, &g_hpfNode);
                break;
            }

            case NOTE_PRESSED: {
                if (event.value >= NOTE_MAX) {
                    printf("*error* incorrect note value");
                    break;
                }

                g_envelopes[event.value].stage = ATTACK;
                g_envelopes[event.value].gain = 0.0f;
                ma_atomic_uint32_set(&g_state.keys[event.value], 1);
                break;
            }

            case NOTE_RELEASED: {
                if (event.value >= NOTE_MAX) {
                    printf("*error* incorrect note value");
                    break;
                }

                Envelope *env = &g_envelopes[event.value];
                if (env->stage != IDLE && env->stage != RELEASE) {
                    env->release_start_gain = env->gain;
                    env->stage = RELEASE;
                }
                break;
            }

            default: {
                printf("event not currently implemented");
                break;
            }
        }
    }

    ma_node_graph_read_pcm_frames(&g_nodeGraph, pOutput, frameCount, NULL);

    void *write_buffer;
    ma_uint32 frames_to_write = frameCount;

    if (ma_pcm_rb_acquire_write(&g_udpBuf, &frames_to_write, (void**)&write_buffer) == MA_SUCCESS) {
        MA_COPY_MEMORY(write_buffer, pOutput, frames_to_write * CHANNELS * sizeof(float));
        ma_pcm_rb_commit_write(&g_udpBuf, frames_to_write);
    }

    (void)pInput;
}

/* ------------------------------------------------------------------------------------------------------------- */

/* Main */

int main(void) {


    ma_result result;

    result = ma_rb_init(sizeof(g_eventBufStore), g_eventBufStore, NULL, &g_eventBuf);
    if (result != MA_SUCCESS) {
        printf("*error* failed to initialize event ring buffer.\n");
        return -1;
    }

    result = ma_pcm_rb_init(FORMAT, CHANNELS, 2 * SAMPLE_RATE, NULL, NULL, &g_udpBuf);
    if (result != MA_SUCCESS) {
        printf("*error* failed to initialize udp ring buffer.\n");
        return -1;
    }

    ma_atomic_uint32_set(&g_state.waveform, (ma_uint32)BASE_TYPE);

    ma_atomic_float_set(&g_state.attack,  0.5f);
    ma_atomic_float_set(&g_state.decay,   0.1f);
    ma_atomic_float_set(&g_state.sustain, 1.0f);
    ma_atomic_float_set(&g_state.release, 1.0f);

    for (int i = 0; i < MAX_NOTES; i++) {
        ma_atomic_uint32_set(&g_state.keys[i], 0);
        g_envelopes[i].stage = IDLE;
        g_envelopes[i].gain = 0.0f;
        g_envelopes[i].release_start_gain = 0.0f;
    }

    ma_atomic_float_set(&g_state.lfo_frequency, 10.0f);
    ma_atomic_float_set(&g_state.lfo_depth, 1.0f);

    ma_atomic_float_set(&g_state.lpf_cutoff, LPF_CUTOFF_MAX);
    ma_atomic_float_set(&g_state.hpf_cutoff, LPF_CUTOFF_MIN);

    ma_atomic_float_set(&g_state.pitch_offset, 0.0f);

    /* Node Graph */
    {
        ma_node_graph_config nodeGraphConfig = ma_node_graph_config_init(CHANNELS);

        result = ma_node_graph_init(&nodeGraphConfig, NULL, &g_nodeGraph);
        if (result != MA_SUCCESS) {
            printf("*error* failed to initialize node graph.\n");
            return -1;
        }
    }

  /* Low Pass Filter */
    {
        ma_lpf_node_config lpfNodeConfig = ma_lpf_node_config_init(CHANNELS, SAMPLE_RATE, LPF_CUTOFF_MAX, LPF_ORDER);

        result = ma_lpf_node_init(&g_nodeGraph, &lpfNodeConfig, NULL, &g_lpfNode);
        if (result != MA_SUCCESS) {
            printf("*error* failed to initialize low pass filter node.\n");
            goto cleanup_graph;
        }

        ma_node_attach_output_bus(&g_lpfNode, 0, ma_node_graph_get_endpoint(&g_nodeGraph), 0);
    }

  /* High Pass Filter */
    {
        ma_hpf_node_config hpfNodeConfig = ma_hpf_node_config_init(CHANNELS, SAMPLE_RATE, HPF_CUTOFF_MIN, HPF_ORDER);

        result = ma_hpf_node_init(&g_nodeGraph, &hpfNodeConfig, NULL, &g_hpfNode);
        if (result != MA_SUCCESS) {
            printf("*error* failed to initialize high pass filter node.\n");
            goto cleanup_lpf;
        }

        ma_node_attach_output_bus(&g_hpfNode, 0, &g_lpfNode, 0);
    }

  /* LFO Node */
    {
        ma_uint32 inputChannels[1] = { CHANNELS };
        ma_uint32 outputChannels[1] = { CHANNELS };

        ma_node_config tremoloConfig = ma_node_config_init();
        tremoloConfig.vtable = &g_tremoloNodeVTable;
        tremoloConfig.pInputChannels = inputChannels;
        tremoloConfig.pOutputChannels = outputChannels;

        result = ma_node_init(&g_nodeGraph, &tremoloConfig, NULL, &g_lfoNode.base);
        if (result != MA_SUCCESS) {
            printf("*error* failed to initialize tremolo node.\n");
            goto cleanup_hpf;
        }

        g_lfoNode.phase = 0.0f;
        g_lfoNode.sample_rate = SAMPLE_RATE;

        ma_node_attach_output_bus(&g_lfoNode, 0, &g_hpfNode, 0);
    }

  /* Waves */
    {
        for (ma_uint32 note = 0; note < MAX_NOTES; note++) {
            ma_waveform_config waveConfig = ma_waveform_config_init(
                FORMAT,
                CHANNELS,
                SAMPLE_RATE,
                BASE_TYPE,
                0.0f,
                frequency_from_midi_note((ma_float)note)
            );

            result = ma_waveform_init(&waveConfig, &g_waves[note]);
            if (result != MA_SUCCESS) {
                printf("*error* failed to initialize wave waveform.\n");
                goto cleanup_lfo;
            }
        }
    }

  /* Wrap wave as a data source node and attach it to the graph */
    {
        ma_uint32 outputChannels[1] = { CHANNELS };

        ma_node_config polyConfig = ma_node_config_init();
        polyConfig.vtable = &g_polyNodeVTable;
        polyConfig.pOutputChannels = outputChannels;

        result = ma_node_init(&g_nodeGraph, &polyConfig, NULL, &g_polyNode);
        if (result != MA_SUCCESS) {
            printf("*error* failed to initialize wave node.\n");
            goto cleanup_lfo;
        }

        ma_node_attach_output_bus(&g_polyNode, 0, &g_lfoNode, 0);
    }

  /* Playback Device */
    {
        ma_device_config deviceConfig;
        ma_device device;

        deviceConfig = ma_device_config_init(ma_device_type_playback);
        deviceConfig.playback.format = FORMAT;
        deviceConfig.playback.channels = CHANNELS;

        deviceConfig.sampleRate = SAMPLE_RATE;
        deviceConfig.dataCallback = data_callback;
        deviceConfig.pUserData = NULL;

        result = ma_device_init(NULL, &deviceConfig, &device);
        if (result != MA_SUCCESS) {
            printf("*error* failed to initialize playback device.\n");
            goto cleanup_wave;
        }

        printf("*info* device name: %s\n", device.playback.name);


        result = ma_device_start(&device);
        if (result != MA_SUCCESS) {
            printf("*error* failed to start playback device.\n");
            ma_device_uninit(&device);
            goto cleanup_wave;
        }

    #ifdef __EMSCRIPTEN__
        emscripten_set_main_loop(main_loop__em, 0, 1);
    #else
        // main
        //
        sleep(2);

        // A simple demo
        // Event c3_on = { NOTE_PRESSED, 48 +12 };
        // Event e3_on = { NOTE_PRESSED, 52 +12 };
        // Event g3_on = { NOTE_PRESSED, 55 +12 };
        // Event b3_on = { NOTE_PRESSED, 59 +12 };

        // push_event(&c3_on);
        // push_event(&e3_on);
        // push_event(&g3_on);
        // push_event(&b3_on);

        // sleep(2);

        // Event c3_off = { NOTE_RELEASED, 48 +12 };
        // Event e3_off = { NOTE_RELEASED, 52 +12 };
        // Event g3_off = { NOTE_RELEASED, 55 +12 };
        // Event b3_off = { NOTE_RELEASED, 59 +12 };

        // push_event(&c3_off);
        // push_event(&e3_off);
        // push_event(&g3_off);
        // push_event(&b3_off);

        pthread_t conf_thread;
        pthread_create(&conf_thread, NULL, poll_conf, NULL);

        pthread_t udp_thread;
        pthread_create(&udp_thread, NULL, send_audio_udp, NULL);

        printf("*info* press enter to quit...\n");
        getchar();
    #endif
        ma_device_uninit(&device);
    }

  cleanup_wave:
    ma_node_uninit(&g_polyNode, NULL);
  cleanup_lfo:
    ma_node_uninit(&g_lfoNode.base, NULL);
  cleanup_hpf:
    ma_hpf_node_uninit(&g_hpfNode, NULL);
  cleanup_lpf:
    ma_lpf_node_uninit(&g_lpfNode, NULL);
  cleanup_graph:
    ma_node_graph_uninit(&g_nodeGraph, NULL);

    ma_rb_uninit(&g_eventBuf);
    return 0;
}
