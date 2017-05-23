#ifdef USEAPI_BELA

#include "m_pd.h"
#include "s_stuff.h"
#include <Bela.h>

struct bela_callback_args_struct
{
    t_sample soundin;
    t_sample soundout;
    t_audiocallback callbackfn;
};

int setup(BelaContext* context, (void*)belaArgs)
{

}

int bela_open_audio(int inchans, int outchans, int rate, t_sample *soundin,
    t_sample *soundout, int framesperbuf, int nbuffers,
    int indeviceno, int outdeviceno, t_audiocallback callbackfn)
{
    BelaInitSettings settings;
    Bela_defaultSettings(&settings);
    bela_callback_args_struct* belaArgs = (bela_callback_args_struct*)malloc(sizeof(bela_callback_args_struct));
    belaArgs->soundin = soundin;
    belaArgs->soundout = soundout;
    belaArgs->callbackfn = callbackfn;
    Bela_initAudio(settings, (void*)belaArgs);
    Bela_startAudio();
}

void render(BelaContext* context, void* userArgs)
{
    printf("Calling render\n");    
}

void cleanup(BelaContext* context, void* userArgs)
{
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
}

void bela_getdevs(char *indevlist, int *nindevs,
    char *outdevlist, int *noutdevs, int *canmulti, 
        int maxndev, int devdescsize)
{
    *nindevs = 1;
    *outdevlist = 1;
    *canmulti = 2;
    sprintf(indevlist, "Bela I/O");
    sprintf(outdevlist, "Bela I/O");
}

void bela_listdevs(void)
{
	post("Bela has usual audio I/O, analog I/O, digital I/O");
}
#endif /* USEAPI_BELA */
