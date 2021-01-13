#ifdef USEAPI_BELA

#include "m_pd.h"
#include <stdlib.h>
#include "s_stuff.h"
#include <Bela.h>
#include <math.h>
#include <string.h>

typedef struct _bela_callback_args_struct
{
    t_sample* soundin;
    t_sample* soundout;
    int inchans;
    int outchans;
    t_audiocallback callbackfn;
} bela_callback_args;

static void _render(BelaContext* context, void* userArgs);
static void _cleanup(BelaContext* context, void* userArgs);

int bela_open_audio(int inchans, int outchans, int rate, t_sample *soundin,
    t_sample *soundout, int framesperbuf, int nbuffers,
    int indeviceno, int outdeviceno, t_audiocallback callbackfn)
{
    if(callbackfn == NULL)
    {
        perror("Error: no callback provided, Bela requires callback. Unable to start audio\n");
        return 1;
    }
    BelaInitSettings settings;
    Bela_defaultSettings(&settings);
    settings.render = _render;
    settings.cleanup = _cleanup;
    settings.periodSize = framesperbuf;
    if(inchans <= 2)
    {
        settings.useAnalog = 0;
    }
    else
    {
        settings.useAnalog = 1;
        settings.analogOutputsPersist = 0;
        // if there are <= 4 analog channels,
        // we increase their sampling rate.
        // No point in going to 88.2kHz when there are <= 2
        if(inchans <= 6)
        {
            settings.numAnalogInChannels = 4;
            settings.numAnalogOutChannels = 4;
        }
    }
    if(inchans <= 10)
    {
        settings.useDigital = 0;
    }
    else
    {
        //TODO: bela digital
        perror("Bela digital I/O not implemented yet\n");
        return 1;
    }
    bela_callback_args* belaArgs = (bela_callback_args*)malloc(sizeof(bela_callback_args));
    belaArgs->soundin = soundin;
    belaArgs->soundout = soundout;
    belaArgs->inchans = inchans;
    belaArgs->outchans = outchans;
    belaArgs->callbackfn = callbackfn;
    if(Bela_initAudio(&settings, (void*)belaArgs))
    {
        perror("Error while initializing Bela audio\n");
        return 1;
    }

    if(Bela_startAudio())
    {
        perror("Error while starting audio\n");
        return 1;
    }
    return 0;
}

void _render(BelaContext* context, void* userArgs)
{
    bela_callback_args* args = (bela_callback_args*)userArgs;
    t_audiocallback callbackfn = args->callbackfn;
    t_sample* soundin = args->soundin;
    t_sample* soundout = args->soundout;
    int inchans = args->inchans;
    int outchans = args->outchans;
    int inAudioChans = inchans > 2 ? 2 : inchans;
    int outAudioChans = outchans > 2 ? 2 : outchans;
    int inAnalogChans = inchans > 10 ? 8 : inchans - 2;
    int outAnalogChans = outchans > 10 ? 8 : outchans - 2;

    unsigned int f;
    unsigned int ch;
    // audio in
    for(f = 0; f < context->audioFrames; ++f)
    {
        for(ch = 0; ch < inAudioChans; ++ch)
        {
            soundin[ch * context->audioFrames + f] = audioRead(context, f, ch);
        }
    }

    // analog in
    if(context->analogSampleRate == 22050)
    {
        for(f = 0; f < context->analogFrames; ++f)
        {
            for(ch = 0; ch < inAnalogChans; ++ch)
            {
                t_sample in = analogRead(context, f, ch);
                soundin[(ch + 2) * context->audioFrames + f * 2] = in;
                soundin[(ch + 2) * context->audioFrames + f * 2 + 1] = in;
            }
        }
    }
    else if (context->analogSampleRate == 44100)
    {
        for(f = 0; f < context->analogFrames; ++f)
        {
            static int count = 0;
            count++;
            for(ch = 0; ch < inAnalogChans; ++ch)
            {
                t_sample in = analogRead(context, f, ch);
                soundin[(ch + 2)* context->audioFrames + f] = in;
            }
        }
    }
    // call the DSP callback
    (*callbackfn)();

    // audio out
    for(f = 0; f < context->audioFrames; ++f)
    {
        for(ch = 0; ch < outAudioChans; ++ch)
        {
            audioWrite(context, f, ch, soundout[ch * context->audioFrames + f]);
        }
    }

    // analog out
    if(context->analogSampleRate == 22050)
    {
        for(f = 0; f < context->analogFrames; ++f)
        {
            for(ch = 0; ch < outAnalogChans; ++ch)
            {
                t_sample out = soundout[ch * context->audioFrames + f];
                analogWrite(context, f * 2, ch, out);
                analogWrite(context, f * 2 + 1, ch, out);
            }
        }
    }
    else if (context->analogSampleRate == 44100)
    {
        for(f = 0; f < context->analogFrames; ++f)
        {
            for(ch = 0; ch < outAnalogChans; ++ch)
            {
                t_sample out = soundout[ch * context->audioFrames + f];
                analogWrite(context, f, ch, out);
            }
        }
    }
    //TODO: digital

    // clear the outbut buffer: Pd adds into the old buffer
    memset((void*)soundout, 0, (size_t)(context->audioFrames * context->audioOutChannels * sizeof(t_sample)));
}

void _cleanup(BelaContext* context, void* userArgs)
{
    // release the structure that was allocated in bela_open_audio()
    free(userArgs);
}

void bela_close_audio(void)
{
    Bela_stopAudio();
    Bela_cleanupAudio();
}

int bela_send_dacs(void)
{
    printf("bela_send_dacs\n");
    while(!gShouldStop)
        usleep(100000);
    printf("Done bela send dacs\n");
}

void bela_getdevs(char *indevlist, int *nindevs,
    char *outdevlist, int *noutdevs, int *canmulti, 
        int maxndev, int devdescsize)
{
    *nindevs = 1;
    *noutdevs = 1;
    *canmulti = 2;
    sprintf(indevlist, "Bela inputs");
    sprintf(outdevlist, "Bela outputs");
}

void bela_listdevs(void)
{
    printf("bela_listdevs\n");
    post("Bela has usual audio I/O, analog I/O, digital I/O");
}

// wrappers replacing the ones provided by xenomai

#include <sys/time.h>
#include <native/timer.h>

int __wrap_gettimeofday(struct timeval *tv, struct timezone *tz)
{
    // timezone argument is ignored
    RTIME time = rt_timer_read(); 
    const double billion = 1000000000;
    double timeSecs = (double)time / billion;
    tv->tv_sec = (time_t)timeSecs;
    tv->tv_usec = ((float)timeSecs - (time_t)timeSecs) * 1000000.f;
    return 0;
}
#endif /* USEAPI_BELA */
