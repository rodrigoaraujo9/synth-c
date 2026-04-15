#define MINIAUDIO_IMPLEMENTATION
#include "lib/miniaudio.h"
#include <stdio.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
void main_loop__em(void) {}
#endif

/* Data Format */
#define FORMAT ma_format_f32
#define CHANNELS 2
#define SAMPLE_RATE 48000

/* Effect Properties */
#define LPF_BIAS                                                               \
  0.9f /* Higher values make the low-pass filter more audible. Must be between \
          0 and 1. */
#define LPF_CUTOFF_FACTOR 80 /* Higher values = stronger filtering. */
#define LPF_ORDER 8

#define HPF_BIAS (1.0f - LPF_BIAS)
#define HPF_CUTOFF_FACTOR 80 /* Higher values = stronger filtering. */
#define HPF_ORDER 8

static ma_node_graph g_nodeGraph;
static ma_lpf_node g_lpfNode;
static ma_hpf_node g_hpfNode;
static ma_splitter_node g_splitterNode;
static ma_waveform g_sawWave;
static ma_data_source_node g_sawNode;

void data_callback(ma_device *pDevice, void *pOutput, const void *pInput,
                   ma_uint32 frameCount) {
  MA_ASSERT(pDevice->playback.channels == CHANNELS);

  ma_node_graph_read_pcm_frames(&g_nodeGraph, pOutput, frameCount, NULL);

  (void)pInput;
}

int main(void) {
  ma_result result;

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
    ma_lpf_node_config lpfNodeConfig = ma_lpf_node_config_init(
        CHANNELS, SAMPLE_RATE, SAMPLE_RATE / LPF_CUTOFF_FACTOR, LPF_ORDER);

    result = ma_lpf_node_init(&g_nodeGraph, &lpfNodeConfig, NULL, &g_lpfNode);
    if (result != MA_SUCCESS) {
      printf("*error* failed to initialize low pass filter node.\n");
      goto cleanup_graph;
    }

    ma_node_attach_output_bus(&g_lpfNode, 0,
                              ma_node_graph_get_endpoint(&g_nodeGraph), 0);

    ma_node_set_output_bus_volume(&g_lpfNode, 0, LPF_BIAS);
  }

  /* High Pass Filter */
  {
    ma_hpf_node_config hpfNodeConfig = ma_hpf_node_config_init(
        CHANNELS, SAMPLE_RATE, SAMPLE_RATE / HPF_CUTOFF_FACTOR, HPF_ORDER);

    result = ma_hpf_node_init(&g_nodeGraph, &hpfNodeConfig, NULL, &g_hpfNode);
    if (result != MA_SUCCESS) {
      printf("*error* failed to initialize high pass filter node.\n");
      goto cleanup_lpf;
    }

    ma_node_attach_output_bus(&g_hpfNode, 0,
                              ma_node_graph_get_endpoint(&g_nodeGraph), 0);

    ma_node_set_output_bus_volume(&g_hpfNode, 0, HPF_BIAS);
  }

  /* Splitter */
  {
    ma_splitter_node_config splitterNodeConfig =
        ma_splitter_node_config_init(CHANNELS);

    result = ma_splitter_node_init(&g_nodeGraph, &splitterNodeConfig, NULL,
                                   &g_splitterNode);
    if (result != MA_SUCCESS) {
      printf("*error* failed to initialize splitter node.\n");
      goto cleanup_hpf;
    }

    ma_node_attach_output_bus(&g_splitterNode, 0, &g_lpfNode, 0);
    ma_node_attach_output_bus(&g_splitterNode, 1, &g_hpfNode, 0);
  }

  /* Saw Wave */
  {
    ma_waveform_config sawWaveConfig = ma_waveform_config_init(
        FORMAT, CHANNELS, SAMPLE_RATE, ma_waveform_type_sawtooth, 0.2, 220.0);

    result = ma_waveform_init(&sawWaveConfig, &g_sawWave);
    if (result != MA_SUCCESS) {
      printf("*error* failed to initialize saw waveform.\n");
      goto cleanup_splitter;
    }
  }

  /* Wrap saw wave as a data source node and attach it to the graph */
  {
    ma_data_source_node_config sawNodeConfig =
        ma_data_source_node_config_init(&g_sawWave);

    result = ma_data_source_node_init(&g_nodeGraph, &sawNodeConfig, NULL,
                                      &g_sawNode);
    if (result != MA_SUCCESS) {
      printf("*error* failed to initialize saw node.\n");
      goto cleanup_splitter;
    }

    ma_node_attach_output_bus(&g_sawNode, 0, &g_splitterNode, 0);
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
      goto cleanup_saw;
    }

    printf("*info* device name: %s\n", device.playback.name);

    result = ma_device_start(&device);
    if (result != MA_SUCCESS) {
      printf("*error* failed to start playback device.\n");
      ma_device_uninit(&device);
      goto cleanup_saw;
    }

#ifdef __EMSCRIPTEN__
    emscripten_set_main_loop(main_loop__em, 0, 1);
#else
    printf("*info* press enter to quit...\n");
    getchar();
#endif

    ma_device_uninit(&device);
  }

cleanup_saw:
  ma_data_source_node_uninit(&g_sawNode, NULL);
cleanup_splitter:
  ma_splitter_node_uninit(&g_splitterNode, NULL);
cleanup_hpf:
  ma_hpf_node_uninit(&g_hpfNode, NULL);
cleanup_lpf:
  ma_lpf_node_uninit(&g_lpfNode, NULL);
cleanup_graph:
  ma_node_graph_uninit(&g_nodeGraph, NULL);

  return 0;
}
