//! Base for this implementation was:
//! https://miniaud.io/docs/examples/node_graph.html

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

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
    void main_loop__em(void) {}
#endif


/* ------------------------------------------------------------------------------------------------------------- */

/* Config */

// Data Format
#define FORMAT ma_format_f32
#define CHANNELS 2
#define SAMPLE_RATE 48000


// Effect Properties
#define LPF_CUTOFF 10000.0f        // the lower the more evident
#define LPF_ORDER 8                // how agressive freqs beyond cuttof are attenuated (8 is very agressive)

#define HPF_CUTOFF 300.0f          // the higher the more evident
#define HPF_ORDER 8

// Wave Config
#define BASE_AMP 0.2f
#define BASE_FREQ 440.0f // A4
#define BASE_NOTE 69 // A4
#define BASE_TYPE ma_waveform_type_sawtooth

// Input Bounds
#define NOTE_MAX 128
#define MAX_NOTES NOTE_MAX // like keyboard. one key per note

#define PITCH_OFFSET_MAX 2.0f
#define PITCH_OFFSET_MIN -2.0f

#define LFO_FREQUENCY_MAX 20.0f
#define LFO_FREQUENCY_MIN 0.5f

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
    SET_HPF_CUTOFF
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


/* ------------------------------------------------------------------------------------------------------------- */

/* Globals */

static State g_state;

static ma_waveform g_waves[MAX_NOTES];

static ma_node_graph g_nodeGraph;
static ma_lpf_node g_lpfNode;
static ma_hpf_node g_hpfNode;
static Lfo g_lfoNode;
static ma_node_base g_polyNode;

static ma_rb g_eventBuf;
static unsigned char g_eventBufStore[sizeof(Event) * 256];


/* ------------------------------------------------------------------------------------------------------------- */

/* Helper Functions */

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

ma_float frequency_from_midi_note(ma_float note) {
    // c[-1] = 0
    // a[4]  = 69
    // to freq consider base -> 440 (a[4])
    // do the math with distance from a[4]
    const ma_float diff = note - BASE_NOTE;
    const ma_float freq = BASE_FREQ * ma_powf(2.0f, diff/12.0f);

    return freq;
}

int waveform_from_ma_uint32(ma_uint32 value, ma_waveform_type *waveform) {
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

/* Communication Functions */

struct dataDef {
    int pot_val;
} conf;

void *poll_conf() {
    int sfd = open("/dev/cu.usbmodem1101", O_RDWR | O_NOCTTY);
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
    cfsetspeed(&options, 9600);

    options.c_cflag &= ~CSTOPB;
    options.c_cflag |= CLOCAL;
    options.c_cflag |= CREAD;
    options.c_cflag |= CRTSCTS;
    options.c_cc[VTIME] = 0;
    options.c_cc[VMIN] = 1;

    if (tcsetattr(sfd,TCSANOW,&options) < 0) {
        printf("Error setting attributes: %s\n",strerror(errno));
        return NULL;
    }

    tcflush(sfd,TCIFLUSH);

    char c;

    for(;;) {
        char *c_bytes = (char*) &conf;

        for (int i = 0; i < sizeof(conf); i++) {
            if (read(sfd, &c_bytes[i],1) == -1) break;
        }

        const Event e = (Event) {SET_LFO_DEPTH, conf.pot_val};
        push_event(&e);
    }
}

/* Main Functions */

// backend runs this funct on repeat
void data_callback(ma_device *pDevice, void *pOutput, const void *pInput, ma_uint32 frameCount) {
    Event event;

    MA_ASSERT(pDevice->playback.channels == CHANNELS);

    while (pop_event(&event)) {
        switch (event.type) {
            case SET_WAVE: {
                ma_waveform_type waveform;

                if (waveform_from_ma_uint32(event.value, &waveform) != 0){
                    printf("*error* failed to convert uint32 to waveform");
                    break;
                }

                ma_atomic_uint32_set(&g_state.waveform, event.value);
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

                ma_atomic_uint32_set(&g_state.keys[event.value], 1);
                break;
            }

            case NOTE_RELEASED: {
                if (event.value >= NOTE_MAX) {
                    printf("*error* incorrect note value");
                    break;
                }

                ma_atomic_uint32_set(&g_state.keys[event.value], 0);
                break;
            }

            default: {
                printf("event not currently implemented");
                break;
            }
        }
    }

    ma_node_graph_read_pcm_frames(&g_nodeGraph, pOutput, frameCount, NULL);
    (void)pInput;
}

int main(void) {
    ma_result result;

    result = ma_rb_init(sizeof(g_eventBufStore), g_eventBufStore, NULL, &g_eventBuf);
    if (result != MA_SUCCESS) {
        printf("*error* failed to initialize event ring buffer.\n");
        return -1;
    }

    ma_atomic_uint32_set(&g_state.waveform, (ma_uint32)BASE_TYPE);
    ma_atomic_float_set(&g_state.lfo_frequency, 10.0f);
    ma_atomic_float_set(&g_state.lfo_depth, 1.0f);
    ma_atomic_float_set(&g_state.lpf_cutoff, LPF_CUTOFF);
    ma_atomic_float_set(&g_state.hpf_cutoff, HPF_CUTOFF);
    ma_atomic_float_set(&g_state.pitch_offset, 0.0f);
    for (int i = 0; i < MAX_NOTES; i++) {
        ma_atomic_uint32_set(&g_state.keys[i], 0);
    }


    pthread_t conf_thread;
    pthread_create(&conf_thread, NULL, poll_conf, NULL);

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
        ma_lpf_node_config lpfNodeConfig = ma_lpf_node_config_init(CHANNELS, SAMPLE_RATE, LPF_CUTOFF, LPF_ORDER);

        result = ma_lpf_node_init(&g_nodeGraph, &lpfNodeConfig, NULL, &g_lpfNode);
        if (result != MA_SUCCESS) {
            printf("*error* failed to initialize low pass filter node.\n");
            goto cleanup_graph;
        }

        ma_node_attach_output_bus(&g_lpfNode, 0, ma_node_graph_get_endpoint(&g_nodeGraph), 0);
    }

  /* High Pass Filter */
    {
        ma_hpf_node_config hpfNodeConfig = ma_hpf_node_config_init(CHANNELS, SAMPLE_RATE, HPF_CUTOFF, HPF_ORDER);

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

        /* Cool Demo */
        {
            Event wave = { SET_WAVE, 3 };
            Event lfo_freq = { SET_LFO_FREQUENCY, float_as_event_value(16.0f) };
            Event lfo_depth = { SET_LFO_DEPTH, float_as_event_value(0.85f) };
            Event hpf = { SET_HPF_CUTOFF, float_as_event_value(80.0f) };
            Event lpf = { SET_LPF_CUTOFF, float_as_event_value(1800.0f) };

            push_event(&wave);
            push_event(&lfo_freq);
            push_event(&lfo_depth);
            push_event(&hpf);
            push_event(&lpf);

            Event c3_on = { NOTE_PRESSED, 48 };
            Event e3_on = { NOTE_PRESSED, 52 };
            Event g3_on = { NOTE_PRESSED, 55 };
            Event b3_on = { NOTE_PRESSED, 59 };

            push_event(&c3_on);
            push_event(&e3_on);
            push_event(&g3_on);
            push_event(&b3_on);

            for (int i = 0; i <= 100; i++) {
                ma_float t = (ma_float)i / 100.0f;
                ma_float freq = 16.0f + (0.5f - 16.0f) * t;
                ma_float cutoff = 500.0f + (HPF_CUTOFF_MIN - 500.0f) * t;

                Event lfo_ramp = { SET_LFO_FREQUENCY, float_as_event_value(freq) };
                Event hpf_ramp = { SET_HPF_CUTOFF, float_as_event_value(cutoff) };

                push_event(&lfo_ramp);
                push_event(&hpf_ramp);

                usleep(50000);
            }

            Event c3_off = { NOTE_RELEASED, 48 };
            Event e3_off = { NOTE_RELEASED, 52 };
            Event g3_off = { NOTE_RELEASED, 55 };
            Event b3_off = { NOTE_RELEASED, 59 };

            usleep(500000);

            push_event(&c3_off);
            push_event(&e3_off);
            push_event(&g3_off);
            push_event(&b3_off);

            usleep(500000);
        }

    #ifdef __EMSCRIPTEN__
        emscripten_set_main_loop(main_loop__em, 0, 1);
    #else
        //for now this is as was initially. will detect keypresses for now to test.
        // will detect keypress and release and publish it on event buf. asynchronously read event buf (if detects != current active_note value -> act)
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

/* ------------------------------------------------------------------------------------------------------------- */

/* Normalization and Update */

void joystick_to_cutoff(float x) {
    float middle = 512.0f; // adapt
    float offset = x - middle;

    Event lpf, hpf;

    if (offset <= 0) {
        // more drowned as distance form middle grows
        ma_uint32 value = float_as_event_value(middle * LPF_CUTOFF_MAX / LPF_CUTOFF_MIN);
        lpf = (Event){SET_LPF_CUTOFF, value};
        hpf = (Event){SET_HPF_CUTOFF, HPF_CUTOFF_MIN};
    } else if (offset > 0){
        ma_uint32 value = float_as_event_value(middle * HPF_CUTOFF_MIN / HPF_CUTOFF_MAX);
        lpf = (Event){SET_LPF_CUTOFF, LPF_CUTOFF_MAX};
        hpf = (Event){SET_HPF_CUTOFF, value};
    }
    push_event(&lpf);
    push_event(&hpf);
}

void joystick_to_pitch_offset(float y) {
    float middle = 512.0f; // adapt
    float offset = y - middle;

    ma_uint32 value = float_as_event_value(middle * PITCH_OFFSET_MIN / PITCH_OFFSET_MAX);

    Event pitch = {SET_PITCH_OFFSET, value};

    push_event(&pitch);
}

Event ultrassonic_to_lfo_frequency(float raw) {
    // adapt values
    float min = 2.0f;   //cm
    float max = 400.0f; //cm



}

void potenciometer_to_lfo_depth(int raw) {
    // min is 0 for both
    float max = 65930223.0f;
    float value = ((float) raw) / max;
    Event depth = {SET_LFO_DEPTH, value};
    push_event(&depth)
}
