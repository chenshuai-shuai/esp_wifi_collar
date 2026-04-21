idf_component_register(
    SRCS
        "audio_denoise.c"
        "speex_echo_stub.c"
        "third_party/speexdsp/libspeexdsp/preprocess.c"
        "third_party/speexdsp/libspeexdsp/fftwrap.c"
        "third_party/speexdsp/libspeexdsp/filterbank.c"
        "third_party/speexdsp/libspeexdsp/kiss_fft.c"
        "third_party/speexdsp/libspeexdsp/kiss_fftr.c"
    INCLUDE_DIRS
        "include"
        "third_party/speexdsp/include"
        "third_party/speexdsp/libspeexdsp"
    REQUIRES esp_timer log
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE FIXED_POINT USE_KISS_FFT EXPORT=)
