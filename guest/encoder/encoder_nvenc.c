/* Direct NVENC H.264, synchronous on the existing encoder worker. */
#include "encoder_nvenc.h"
#include "vendor/nvEncodeAPI.h"
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>

/* Stable CUDA driver ABI only; these are opaque context/device handles.
 * NVENC owns its input allocation and RGB conversion, so no CUDA kernels or
 * toolkit libraries are needed. The system NVIDIA driver supplies both DSOs. */
struct np_h264_nvenc {
    void *cuda_library, *nvenc_library, *context, *encoder;
    int device;
    int (*cuInit)(unsigned);
    int (*cuDeviceGet)(int *, int);
    int (*cuDevicePrimaryCtxRetain)(void **, int);
    int (*cuDevicePrimaryCtxRelease)(int);
    int (*cuCtxPushCurrent)(void *);
    int (*cuCtxPopCurrent)(void **);
    NV_ENCODE_API_FUNCTION_LIST api;
    /* NVENC requires at least four input buffers even with zero B frames.
     * Only one is submitted at a time; allocation does not add a frame queue. */
    NV_ENC_INPUT_PTR input[4];
    NV_ENC_OUTPUT_PTR output[4];
    unsigned slot;
    int width, height;
    bool initialized, pending;
    uint64_t frame;
};

bool np_h264_nvenc_supports_size(int width, int height)
{
    return width >= 32 && height >= 32 && width <= 4096 && height <= 4096 &&
        !(width & 1) && !(height & 1);
}

static bool status_ok(NVENCSTATUS status, const char *operation)
{
    if (status == NV_ENC_SUCCESS) return true;
    fprintf(stderr, "[encoder] NVENC %s failed status=%d\n", operation, status);
    return false;
}
#define NV_CALL(e, name, ...) status_ok((e)->api.name((e)->encoder, __VA_ARGS__), #name)

void np_h264_nvenc_destroy(struct np_h264_nvenc *e)
{
    if (!e) return;
    if (e->context && e->cuCtxPushCurrent(e->context) == 0) {
        if (e->initialized) {
            NV_ENC_PIC_PARAMS eos = { .version = NV_ENC_PIC_PARAMS_VER,
                .encodePicFlags = NV_ENC_PIC_FLAG_EOS };
            e->api.nvEncEncodePicture(e->encoder, &eos);
            if (e->pending) {
                NV_ENC_LOCK_BITSTREAM lock = { .version = NV_ENC_LOCK_BITSTREAM_VER,
                    .outputBitstream = e->output[e->slot] };
                if (e->api.nvEncLockBitstream(e->encoder, &lock) == NV_ENC_SUCCESS)
                    e->api.nvEncUnlockBitstream(e->encoder, lock.outputBitstream);
            }
        }
        for (unsigned i = 0; i < 4; i++) {
            if (e->input[i]) e->api.nvEncDestroyInputBuffer(e->encoder, e->input[i]);
            if (e->output[i]) e->api.nvEncDestroyBitstreamBuffer(e->encoder, e->output[i]);
        }
        if (e->encoder) e->api.nvEncDestroyEncoder(e->encoder);
        void *previous; e->cuCtxPopCurrent(&previous);
    }
    if (e->context) e->cuDevicePrimaryCtxRelease(e->device);
    if (e->nvenc_library) dlclose(e->nvenc_library);
    if (e->cuda_library) dlclose(e->cuda_library);
    free(e);
}

struct np_h264_nvenc *np_h264_nvenc_create(int width, int height)
{
    if (!np_h264_nvenc_supports_size(width, height)) return NULL;
    struct np_h264_nvenc *e = calloc(1, sizeof(*e));
    if (!e) return NULL;
    e->width = width; e->height = height;
    e->cuda_library = dlopen("libcuda.so.1", RTLD_NOW | RTLD_LOCAL);
    e->nvenc_library = dlopen("libnvidia-encode.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!e->cuda_library || !e->nvenc_library) goto fail;
#define CUDA_SYMBOL(field, symbol) do { \
    *(void **)(&e->field) = dlsym(e->cuda_library, symbol); \
    if (!e->field) goto fail; \
} while (0)
    CUDA_SYMBOL(cuInit, "cuInit");
    CUDA_SYMBOL(cuDeviceGet, "cuDeviceGet");
    CUDA_SYMBOL(cuDevicePrimaryCtxRetain, "cuDevicePrimaryCtxRetain");
    CUDA_SYMBOL(cuDevicePrimaryCtxRelease, "cuDevicePrimaryCtxRelease_v2");
    CUDA_SYMBOL(cuCtxPushCurrent, "cuCtxPushCurrent_v2");
    CUDA_SYMBOL(cuCtxPopCurrent, "cuCtxPopCurrent_v2");
#undef CUDA_SYMBOL
    NVENCSTATUS (*create_api)(NV_ENCODE_API_FUNCTION_LIST *) =
        dlsym(e->nvenc_library, "NvEncodeAPICreateInstance");
    NVENCSTATUS (*max_version)(uint32_t *) =
        dlsym(e->nvenc_library, "NvEncodeAPIGetMaxSupportedVersion");
    uint32_t supported = 0;
    if (!create_api || !max_version || max_version(&supported) != NV_ENC_SUCCESS ||
        supported < ((NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION)) goto fail;
    e->api.version = NV_ENCODE_API_FUNCTION_LIST_VER;
    if (create_api(&e->api) != NV_ENC_SUCCESS) goto fail;
    /* ponytail: use device zero; add device selection when multiple GPUs need it. */
    if (e->cuInit(0) || e->cuDeviceGet(&e->device, 0) ||
        e->cuDevicePrimaryCtxRetain(&e->context, e->device)) goto fail;
    if (e->cuCtxPushCurrent(e->context)) goto fail;
    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS open = {
        .version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER,
        .deviceType = NV_ENC_DEVICE_TYPE_CUDA, .device = e->context,
        .apiVersion = NVENCAPI_VERSION };
    if (!status_ok(e->api.nvEncOpenEncodeSessionEx(&open, &e->encoder), "open")) goto pop_fail;
    NV_ENC_BUFFER_FORMAT formats[64]; uint32_t count = 0;
    if (!NV_CALL(e, nvEncGetInputFormats, NV_ENC_CODEC_H264_GUID, formats, 64, &count) || count > 64) goto pop_fail;
    bool bgra_supported = false;
    for (uint32_t i = 0; i < count; i++) bgra_supported |= formats[i] == NV_ENC_BUFFER_FORMAT_ARGB;
    if (!bgra_supported) goto pop_fail;
    NV_ENC_PRESET_CONFIG preset = { .version = NV_ENC_PRESET_CONFIG_VER,
        .presetCfg = { .version = NV_ENC_CONFIG_VER } };
    if (!NV_CALL(e, nvEncGetEncodePresetConfigEx, NV_ENC_CODEC_H264_GUID,
        NV_ENC_PRESET_P1_GUID, NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY, &preset)) goto pop_fail;
    NV_ENC_CONFIG config = preset.presetCfg;
    config.profileGUID = NV_ENC_H264_PROFILE_HIGH_GUID;
    config.gopLength = 120; config.frameIntervalP = 1;
    config.frameFieldMode = NV_ENC_PARAMS_FRAME_FIELD_MODE_FRAME;
    config.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
    config.rcParams.averageBitRate = config.rcParams.maxBitRate = 4000000;
    config.rcParams.vbvBufferSize = config.rcParams.vbvInitialDelay = 4000000 / 60;
    config.rcParams.enableLookahead = 0; config.rcParams.lookaheadDepth = 0;
    config.rcParams.enableAQ = config.rcParams.enableTemporalAQ = 0;
    config.rcParams.zeroReorderDelay = 1;
    NV_ENC_CONFIG_H264 *h264 = &config.encodeCodecConfig.h264Config;
    h264->idrPeriod = 120; h264->repeatSPSPPS = 1;
    h264->chromaFormatIDC = 1; h264->sliceMode = 3; h264->sliceModeData = 1;
    h264->h264VUIParameters.videoSignalTypePresentFlag = 1;
    h264->h264VUIParameters.videoFormat = 5;
    h264->h264VUIParameters.videoFullRangeFlag = 0;
    h264->h264VUIParameters.colourDescriptionPresentFlag = 1;
    h264->h264VUIParameters.colourPrimaries = 6;
    h264->h264VUIParameters.transferCharacteristics = 6;
    h264->h264VUIParameters.colourMatrix = 6;
    NV_ENC_INITIALIZE_PARAMS init = {
        .version = NV_ENC_INITIALIZE_PARAMS_VER,
        .encodeGUID = NV_ENC_CODEC_H264_GUID, .presetGUID = NV_ENC_PRESET_P1_GUID,
        .encodeWidth = width, .encodeHeight = height, .darWidth = width, .darHeight = height,
        .frameRateNum = 60, .frameRateDen = 1, .enableEncodeAsync = 0, .enablePTD = 1,
        .encodeConfig = &config, .maxEncodeWidth = width, .maxEncodeHeight = height,
        .tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY };
    if (!NV_CALL(e, nvEncInitializeEncoder, &init)) goto pop_fail;
    e->initialized = true;
    for (unsigned i = 0; i < 4; i++) {
        NV_ENC_CREATE_INPUT_BUFFER input = { .version = NV_ENC_CREATE_INPUT_BUFFER_VER,
            .width = width, .height = height, .bufferFmt = NV_ENC_BUFFER_FORMAT_ARGB };
        if (!NV_CALL(e, nvEncCreateInputBuffer, &input)) goto pop_fail;
        e->input[i] = input.inputBuffer;
        NV_ENC_CREATE_BITSTREAM_BUFFER output = { .version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER };
        if (!NV_CALL(e, nvEncCreateBitstreamBuffer, &output)) goto pop_fail;
        e->output[i] = output.bitstreamBuffer;
    }
    void *previous; e->cuCtxPopCurrent(&previous);
    fprintf(stderr, "[encoder] H.264 NVENC hardware %dx%d, BGRA input, no B frames/lookahead\n", width, height);
    return e;
pop_fail: {
    void *previous; e->cuCtxPopCurrent(&previous);
}
fail:
    np_h264_nvenc_destroy(e);
    return NULL;
}

bool np_h264_nvenc_encode(struct np_h264_nvenc *e, const uint8_t *bgra, int stride,
    bool keyframe, uint8_t **packet, size_t *size)
{
    if (!e || !bgra || stride < e->width * 4 || !packet || !size || e->pending) return false;
    *packet = NULL; *size = 0;
    if (e->cuCtxPushCurrent(e->context)) return false;
    bool ok = false;
    NV_ENC_LOCK_INPUT_BUFFER input = { .version = NV_ENC_LOCK_INPUT_BUFFER_VER,
        .inputBuffer = e->input[e->slot] };
    if (!NV_CALL(e, nvEncLockInputBuffer, &input)) goto finish;
    if (!input.bufferDataPtr || input.pitch < (uint32_t)e->width * 4) {
        e->api.nvEncUnlockInputBuffer(e->encoder, input.inputBuffer); goto finish;
    }
    for (int y = 0; y < e->height; y++)
        memcpy((uint8_t *)input.bufferDataPtr + (size_t)y * input.pitch,
            bgra + (size_t)y * stride, (size_t)e->width * 4);
    if (!NV_CALL(e, nvEncUnlockInputBuffer, input.inputBuffer)) goto finish;
    NV_ENC_PIC_PARAMS pic = { .version = NV_ENC_PIC_PARAMS_VER,
        .inputWidth = e->width, .inputHeight = e->height, .inputPitch = input.pitch,
        .inputBuffer = e->input[e->slot], .outputBitstream = e->output[e->slot],
        .bufferFmt = NV_ENC_BUFFER_FORMAT_ARGB, .pictureStruct = NV_ENC_PIC_STRUCT_FRAME,
        .inputTimeStamp = e->frame++, .inputDuration = 1,
        .encodePicFlags = keyframe ? NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS : 0 };
    NVENCSTATUS status = e->api.nvEncEncodePicture(e->encoder, &pic);
    e->pending = status == NV_ENC_SUCCESS || status == NV_ENC_ERR_NEED_MORE_INPUT;
    if (!status_ok(status, "encode")) goto finish;
    NV_ENC_LOCK_BITSTREAM output = { .version = NV_ENC_LOCK_BITSTREAM_VER,
        .outputBitstream = e->output[e->slot] };
    if (!NV_CALL(e, nvEncLockBitstream, &output)) goto finish;
    if (output.bitstreamBufferPtr && output.bitstreamSizeInBytes &&
        output.bitstreamSizeInBytes <= 32u * 1024u * 1024u) {
        *packet = malloc(output.bitstreamSizeInBytes);
        if (*packet) { *size = output.bitstreamSizeInBytes; memcpy(*packet, output.bitstreamBufferPtr, *size); ok = true; }
    }
    if (!NV_CALL(e, nvEncUnlockBitstream, output.outputBitstream)) ok = false;
    e->pending = false;
    e->slot = (e->slot + 1) % 4;
finish: {
    void *previous;
    if (e->cuCtxPopCurrent(&previous)) ok = false;
}
    if (!ok) { free(*packet); *packet = NULL; *size = 0; }
    return ok;
}
