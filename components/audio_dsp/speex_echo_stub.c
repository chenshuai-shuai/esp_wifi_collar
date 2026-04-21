#include "arch.h"
#include "speex/speex_echo.h"

void speex_echo_get_residual(SpeexEchoState *st, spx_word32_t *residual_echo, int len)
{
    (void)st;
    for (int i = 0; i < len; ++i) {
        residual_echo[i] = 0;
    }
}
