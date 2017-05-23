#ifdef USEAPI_BELA

#include "m_pd.h"
#include <stdlib.h>
#include "s_stuff.h"
#include <Bela.h>

typedef struct _bela_callback_args_struct
{
    t_sample* soundin;
    t_sample* soundout;
    t_audiocallback callbackfn;
} bela_callback_args;

bool setup(BelaContext* context, void* belaArgs)
{
    printf("bela setup\n");
}

int bela_open_audio(int inchans, int outchans, int rate, t_sample *soundin,
    t_sample *soundout, int framesperbuf, int nbuffers,
    int indeviceno, int outdeviceno, t_audiocallback callbackfn)
{
	if(callbackfn == NULL)
	{
		printf("Error: no callback provided, Bela requires callback. Unable to start audio\n");
		return 1;
	}
    printf("bela_open_audio inchans %d outchans %d rate %d soundin %p soundout %p framesperbuf %d nbuffers %d indeviceno %d, outdeviceno %d, callbackfn %p\n",
		inchans, outchans, rate, soundin,
		soundout, framesperbuf, nbuffers,
		indeviceno, outdeviceno, callbackfn	
	);
    BelaInitSettings settings;
    Bela_defaultSettings(&settings);
	settings.periodSize = framesperbuf;
    settings.headphoneLevel = -12;
    bela_callback_args* belaArgs = (bela_callback_args*)malloc(sizeof(bela_callback_args));
    belaArgs->soundin = soundin;
    belaArgs->soundout = soundout;
    belaArgs->callbackfn = callbackfn;
    Bela_initAudio(&settings, (void*)belaArgs);

    if(Bela_startAudio())
		return 1;
	return 0;
}

void render(BelaContext* context, void* userArgs)
{
    bela_callback_args* args = (bela_callback_args*)userArgs;
    t_audiocallback callbackfn = args->callbackfn;
    t_sample* soundin = args->soundin;
    t_sample* soundout = args->soundout;
    (*callbackfn)();
    unsigned int f;
    unsigned int ch;
    unsigned int n;
    static int count = 0;
    count++;
    if(count % 1000 == 0)
    {
        for(n = 0; n < context->audioFrames * context->audioOutChannels; ++n)
        {
            float out = soundout[n];
            if(out < 0.00001 && out > -0.00001)
                continue;
            __real_printf("[%d] %.6f   ", n, out);
            if(n % 2 == 1)
                __real_printf("\n");
        }
    }
    if(1)
    for(f = 0; f < context->audioFrames; ++f)
    {
        for(ch = 0; ch < context->audioOutChannels; ++ch)
        {

            //audioWrite(context, f, ch, soundout[ch * context->audioFrames + f]);
            audioWrite(context, f, ch, 0.001 * soundout[f * context->audioOutChannels + ch]);
        }
    }
}

void cleanup(BelaContext* context, void* userArgs)
{
    printf("bela calling cleanup\n");
    free(userArgs);
}

void bela_close_audio(void)
{
    printf("bela_close_audio\n");
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
    printf("bela_getdevs\n");
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
