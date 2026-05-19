/*
 * resid_wrap.cpp  –  thin C-callable wrapper around the reSID library.
 *
 * Compile with:
 *   g++ -O2 -c resid_wrap.cpp -o resid_wrap.o
 *
 * Then link with supersynth.o:
 *   g++ -O2 supersynth.o resid_wrap.o -lncurses -lasound -lresid -lm -o supersynth
 *
 * reSID homepage: https://sourceforge.net/projects/vice-emu/ (bundled with VICE)
 * Debian/Ubuntu:  sudo apt install libresid-builder-dev   OR  libsidplayfp-dev
 *
 * If libresid-builder is not available, try the standalone reSID package:
 *   sudo apt install resid   (or build from source)
 *
 * Header locations vary by distro; adjust the include path as needed.
 */

/* Try the most common header locations in order */
#if __has_include(<resid/sid.h>)
#  include <resid/sid.h>
   /* sidplay-libs 2.x reSID: global namespace, no reSID:: prefix */
#elif __has_include(<sid.h>)
#  include <sid.h>
#else
#  error "Cannot find reSID headers. Install libresid-dev or adjust include path."
#endif

#include <cstring>
#include <cstdint>

extern "C" {

void *resid_new(void)
{
    SID *s = new SID();
    s->set_chip_model(MOS6581);
    s->set_sampling_parameters(985248, SAMPLE_FAST, 44100);
    s->enable_filter(true);
    return static_cast<void *>(s);
}

void resid_free(void *sid)
{
    delete static_cast<SID *>(sid);
}

void resid_reset(void *sid)
{
    static_cast<SID *>(sid)->reset();
}

int resid_write(void *sid, int reg, int val)
{
    SID *s = static_cast<SID *>(sid);
    s->write(static_cast<reg8>(reg), static_cast<reg8>(val & 0xFF));
    return 0;
}

/*
 * Clock the SID for `cycles` cycles and produce `*count` 16-bit samples.
 * On return *count contains the actual number of samples produced.
 */
void resid_clock_delta(void *sid, int cycles, short *buf, int *count)
{
    SID *s = static_cast<SID *>(sid);
    /* reSID clock() with delta_t returns samples; buf must be big enough */
    cycle_count delta = static_cast<cycle_count>(cycles);
    int n = s->clock(delta, buf, *count);
    *count = n;
}

} /* extern "C" */
