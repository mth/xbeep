#if 0
cc -o xbeep -W -O3 $0 -lX11 -lasound -lm
exit $?
#endif

#include <stdio.h>
#include <stdlib.h>
#include <syslog.h>
#include <math.h>
#include <X11/Xlib.h>
#include <X11/XKBlib.h>
#include <alsa/asoundlib.h>

#define MAX_SAMPLES 8192

static const char *alsa_device = "default";
static const unsigned sample_rate = 48000;
static const int duration_limit = 20000; // 20 seconds
static int xkb_event_code;

static int beep(int percent, int pitch, int duration) {
    int16_t buffer[MAX_SAMPLES];
    unsigned test_endian = 1;
    snd_pcm_format_t fmt = ((char*) &test_endian) ? SND_PCM_FORMAT_S16_LE : SND_PCM_FORMAT_S16_BE;

    if (pitch <= 20 || pitch >= 20000 || percent <= 0 || duration <= 0)
        return 1; // don't try to play unplayable bells

    // https://www.alsa-project.org/alsa-doc/alsa-lib/_2test_2pcm_min_8c-example.html
    // https://alexvia.com/post/003_alsa_playback/
    // https://www.linuxjournal.com/article/6735
    // https://www.alsa-project.org/alsa-doc/alsa-lib/pcm.html
    //printf("Bell percent=%d pitch=%d duration=%d\n", percent, pitch, duration);

    snd_pcm_t *handle;
    int err = snd_pcm_open(&handle, alsa_device,
                           SND_PCM_STREAM_PLAYBACK, 0);
    if (err >= 0)
        err = snd_pcm_set_params(handle,
                ((char*) &test_endian) ? SND_PCM_FORMAT_S16_LE
                                       : SND_PCM_FORMAT_S16_BE,
                SND_PCM_ACCESS_RW_INTERLEAVED,
                1, sample_rate, 1, 200000); // 1 channel, latency 200ms
    if (err < 0) {
        syslog(LOG_ERR, "ALSA device %s open error: %s\n",
               alsa_device, snd_strerror(err));
        return 0;
    }

    if (duration > duration_limit) {
        syslog(LOG_DEBUG, "Limiting bell duration %d to %d",
                          duration, duration_limit);
        duration = duration_limit;
    }
    if (percent > 100)
        percent = 100;

    double sample_step = (double) pitch / (double) sample_rate;
    int sample_count = round(round(MAX_SAMPLES * sample_step - 1) / sample_step);
    // approximate frame_count to half periods to avoid pop at the end
    double half_step = sample_step * 2;
    long frame_count = round(round(duration * sample_rate / 1000 * half_step) / half_step);

    if (sample_count > frame_count)
	sample_count = frame_count;

    sample_step *= 2 * M_PI;
    double amplitude = 327.67 * percent;

    for (int i = 0; i < sample_count; ++i) {
        buffer[i] = amplitude * sin(i * sample_step);
        //printf("%d\n", buffer[i]);
    }

    do {
        uint16_t *buf_p = buffer;
        snd_pcm_uframes_t have_frames = sample_count;
        if (have_frames > (unsigned long) frame_count)
            have_frames = frame_count;
        // printf("amplitude=%lf step=1/%lf sample_count=%d frame_count=%ld have_frames=%ld\n",
        //        amplitude, 1/sample_step, sample_count, frame_count, have_frames);

        while (err >= 0 && have_frames > 0) {
            // frames sent to ALSA
            snd_pcm_sframes_t sent = snd_pcm_writei(handle, buf_p, have_frames);
            if (sent < 0) {
                err = snd_pcm_recover(handle, sent, 0);
                sent = 0;
            }
            have_frames -= sent;
            buf_p += sent;
        }
        frame_count -= sample_count;
    } while (err >= 0 && frame_count > 0);

    if (err < 0 || (err = snd_pcm_drain(handle)) < 0)
        syslog(LOG_ERR, "ALSA playback error: %s\n", snd_strerror(err));
    snd_pcm_close(handle);
    return 1;
}

static Display* open_display() {
    int major = XkbMajorVersion;
    int minor = XkbMinorVersion;
    int error;

    Display *dpy = XkbOpenDisplay(NULL, &xkb_event_code, NULL,
                                  &major, &minor, &error);
    if (dpy)
        return dpy;

    switch (error) {
    case XkbOD_BadLibraryVersion:
        fprintf(stderr, "XKB version %d.%d don't match expected %d.%d\n",
                major, minor, XkbMajorVersion, XkbMinorVersion);
        break;
    case XkbOD_ConnectionRefused:
        fputs("Cannot open display\n", stderr);
        break;
    case XkbOD_NonXkbServer:
        fputs("X server has no XKB extension\n", stderr);
        break;
    case XkbOD_BadServerVersion:
        fprintf(stderr, "X server XKB version %d.%d don't match expected %d.%d\n",
                major, minor, XkbMajorVersion, XkbMinorVersion);
        break;
    default:
        fprintf(stderr, "XkbOpenDisplay error %d\n", error);
    }
    exit(1);
}

int main(int argc, const char** argv) {
    if (argc > 1 && (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help"))) {
        puts("xbeep\n\nListens for XkbBellNotify events and plays beeps via ALSA device");
        return 1;
    }
    openlog("xbeep", LOG_PID, LOG_USER);

    Display *dpy = open_display();
    if (!XkbSelectEvents(dpy, XkbUseCoreKbd, XkbBellNotifyMask,
                         XkbBellNotifyMask)) {
        fputs("Cannot select XkbBellNotify events\n", stderr);
        return XCloseDisplay(dpy), 1;
    }

    XkbDescPtr xkb =
        XkbGetKeyboard(dpy, XkbGBN_AllComponentsMask, XkbUseCoreKbd);
    if (!XkbChangeEnabledControls(dpy, XkbUseCoreKbd, XkbAudibleBellMask, 0)) {
        fputs("Cannot disable audible bell\n", stderr);
        return XCloseDisplay(dpy), 1;
    }

    int controls = XkbAudibleBellMask;
    int values = XkbAudibleBellMask;
    if (!XkbSetAutoResetControls(dpy, XkbAudibleBellMask, &controls, &values))
        fputs("XkbSetAutoResetControls(XkbAudibleBellMask) failed\n", stderr);

    for (XkbEvent ev;;) {
        XNextEvent(dpy, &ev.core);
        if (ev.type == xkb_event_code && ev.any.xkb_type == XkbBellNotify &&
            !beep(ev.bell.percent, ev.bell.pitch, ev.bell.duration)) {
            XkbForceDeviceBell(dpy, ev.bell.device, ev.bell.bell_class,
                               ev.bell.bell_id, ev.bell.percent);
        }
    }
}
