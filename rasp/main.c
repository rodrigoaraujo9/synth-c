//! Base for this implementation was:
//! https://miniaud.io/docs/examples/node_graph.html

#define MINIAUDIO_IMPLEMENTATION
#include "lib/miniaudio.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

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
#define LPF_BIAS 0.5f              // higher values make the low-pass filter more audible. Must be between 0 and 1.
#define LPF_CUTOFF 100.0f          // the lower the more evident
#define LPF_ORDER 8                // how agressive freqs beyond cuttof are attenuated (8 is very agressive)

#define HPF_BIAS (1.0f - LPF_BIAS) // inverse to lpf bias for now
#define HPF_CUTOFF 300.0f          // the higher the more evident
#define HPF_ORDER 8

// Wave Config
#define BASE_AMP 0.2f
#define BASE_FREQ 440.0f // A4
#define BASE_NOTE 69 // A4
#define BASE_TYPE ma_waveform_type_sawtooth

// Input Bounds
#define NOTE_MAX 128

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
    ma_atomic_uint32   waveform;       // [0..4] will be mapped to ma_waveform_type

    ma_atomic_uint32   note_is_active; // 0 or 1
    ma_atomic_uint32   note;           // MIDI note number -> base

    ma_atomic_float    pitch_offset;   // Semi-tones makes more sense for logic but less for readability

    ma_atomic_float    lfo_frequency;  // Hz
    ma_atomic_float    lfo_depth;      // [0..1]

    ma_atomic_float    lpf_cutoff;     // Hz
    ma_atomic_float    hpf_cutoff;     // Hz
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

/* ------------------------------------------------------------------------------------------------------------- */

/* Globals */

static State g_state;

static ma_waveform g_wave;

static ma_node_graph g_nodeGraph;
static ma_lpf_node g_lpfNode;
static ma_hpf_node g_hpfNode;
static ma_splitter_node g_splitterNode;
static ma_data_source_node g_waveNode;
static ma_float g_lfo_phase;

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

/// Updates LFO phase and returns amplitude multiplier for current phase
ma_float lfo_step(ma_float frequency, ma_float depth, ma_uint32 frameCount) {
    ma_float value = (1.0f - depth) + (depth * ((1.0f + ma_sinf(2.0f * MA_PI * g_lfo_phase)) / 2.0f));

    g_lfo_phase += frequency * ((ma_float)frameCount / (ma_float)SAMPLE_RATE);

    while (g_lfo_phase >= 1.0f) {
        g_lfo_phase -= 1.0f;
    }

    return value;
}

/* Main Functions */

// backend runs this funct on repeat
void data_callback(ma_device *pDevice, void *pOutput, const void *pInput, ma_uint32 frameCount) {
    Event event;

    MA_ASSERT(pDevice->playback.channels == CHANNELS);

    while (pop_event(&event)) {
        // right now they are operating with direct values
        // later maybe also responsible for transformation of raw arduino values
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
                break;
            }

            case SET_HPF_CUTOFF: {
                ma_float cutoff = event_value_as_float(event.value);

                if (cutoff > HPF_CUTOFF_MAX || cutoff < HPF_CUTOFF_MIN) {
                    printf("*error* out of bounds HPF cutoff value on SET_HPF_CUTOFF event");
                    break;
                }

                ma_atomic_float_set(&g_state.hpf_cutoff, cutoff);
                break;
            }

            case NOTE_PRESSED: {
                if (event.value > NOTE_MAX) {
                    printf("*error* incorrect note value");
                    break;
                }

                ma_atomic_uint32_set(&g_state.note, event.value);
                ma_atomic_uint32_set(&g_state.note_is_active, 1);
                break;
            }

            case NOTE_RELEASED: {
                const ma_uint32 active_note = ma_atomic_uint32_get(&g_state.note);

                if (event.value == active_note){
                    ma_atomic_uint32_set(&g_state.note_is_active, 0);
                }
                break;
            }

            default: {
                printf("event not currently implemented");
                break;
            }
        }
    }

    {
        const ma_waveform_type waveform = (ma_waveform_type)ma_atomic_uint32_get(&g_state.waveform);
        const ma_float note = (ma_float)ma_atomic_uint32_get(&g_state.note);
        const ma_float note_is_active = ma_atomic_uint32_get(&g_state.note_is_active);
        const ma_float offset = ma_atomic_float_get(&g_state.pitch_offset);
        const ma_float frequency = frequency_from_midi_note(note + offset);
        const ma_float lfo_freq = ma_atomic_float_get(&g_state.lfo_frequency);
        const ma_float lfo_depth = ma_atomic_float_get(&g_state.lfo_depth);

        ma_float lfo_gain = lfo_step(lfo_freq, lfo_depth, frameCount);

        ma_waveform_set_type(&g_wave, waveform);
        ma_waveform_set_frequency(&g_wave, frequency);
        ma_waveform_set_amplitude(&g_wave, note_is_active ? BASE_AMP * lfo_gain : 0.0f);
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
    ma_atomic_uint32_set(&g_state.note_is_active, 0);
    ma_atomic_uint32_set(&g_state.note, 0);

    g_lfo_phase = 0.0f;

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
        ma_node_set_output_bus_volume(&g_lpfNode, 0, LPF_BIAS);
    }

  /* High Pass Filter */
    {
        ma_hpf_node_config hpfNodeConfig = ma_hpf_node_config_init(CHANNELS, SAMPLE_RATE, HPF_CUTOFF, HPF_ORDER);

        result = ma_hpf_node_init(&g_nodeGraph, &hpfNodeConfig, NULL, &g_hpfNode);
        if (result != MA_SUCCESS) {
            printf("*error* failed to initialize high pass filter node.\n");
            goto cleanup_lpf;
        }

        ma_node_attach_output_bus(&g_hpfNode, 0, ma_node_graph_get_endpoint(&g_nodeGraph), 0);
        ma_node_set_output_bus_volume(&g_hpfNode, 0, HPF_BIAS);
    }

  /* Splitter */
    {
        ma_splitter_node_config splitterNodeConfig = ma_splitter_node_config_init(CHANNELS);

        result = ma_splitter_node_init(&g_nodeGraph, &splitterNodeConfig, NULL, &g_splitterNode);
        if (result != MA_SUCCESS) {
            printf("*error* failed to initialize splitter node.\n");
            goto cleanup_hpf;
        }

        ma_node_attach_output_bus(&g_splitterNode, 0, &g_lpfNode, 0);
        ma_node_attach_output_bus(&g_splitterNode, 1, &g_hpfNode, 0);
    }

  /* Wave */
    {
        ma_waveform_config waveConfig = ma_waveform_config_init(FORMAT, CHANNELS, SAMPLE_RATE, BASE_TYPE, BASE_AMP, BASE_FREQ);

        result = ma_waveform_init(&waveConfig, &g_wave);
        if (result != MA_SUCCESS) {
            printf("*error* failed to initialize wave waveform.\n");
            goto cleanup_splitter;
        }
    }

  /* Wrap wave as a data source node and attach it to the graph */
    {
        ma_data_source_node_config waveNodeConfig = ma_data_source_node_config_init(&g_wave);

        result = ma_data_source_node_init(&g_nodeGraph, &waveNodeConfig, NULL, &g_waveNode);
        if (result != MA_SUCCESS) {
            printf("*error* failed to initialize wave node.\n");
            goto cleanup_splitter;
        }

        ma_node_attach_output_bus(&g_waveNode, 0, &g_splitterNode, 0);
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

        // test to see that it is reading the buf and executing the rpcs
        {
            Event on = { NOTE_PRESSED, BASE_NOTE };   // A4
            Event off = { NOTE_RELEASED, BASE_NOTE }; // release same note

            if (!push_event(&on)) {
                printf("*error* failed to push NOTE_PRESSED event.\n");
            } else {
                printf("*info* NOTE_PRESSED sent.\n");
            }

            sleep(2);

            if (!push_event(&off)) {
                printf("*error* failed to push NOTE_RELEASED event.\n");
            } else {
                printf("*info* NOTE_RELEASED sent.\n");
            }
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
    ma_data_source_node_uninit(&g_waveNode, NULL);
  cleanup_splitter:
    ma_splitter_node_uninit(&g_splitterNode, NULL);
  cleanup_hpf:
    ma_hpf_node_uninit(&g_hpfNode, NULL);
  cleanup_lpf:
    ma_lpf_node_uninit(&g_lpfNode, NULL);
  cleanup_graph:
    ma_node_graph_uninit(&g_nodeGraph, NULL);

    ma_rb_uninit(&g_eventBuf);
    return 0;
}
