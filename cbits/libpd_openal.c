#if defined(_WIN32)
#include "AL/al.h"
#include "AL/alc.h"
#else
#include <OpenAL/al.h>
#include <OpenAL/alc.h>
#include <OpenAL/MacOSX_OALExtensions.h>
#endif

#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>
#include <math.h>
#include <pthread.h>
#include <time.h>
#include <stdbool.h>

#include "z_libpd.h"
#include "Sound/Pd/Internal_stub.h"

// From openal_soft_reverb.c or openal_mac_reverb.c
// (we link one or the other per platform)
int add_reverb(ALuint* allSourceIDs, int numSourceIDs);

#define NUM_BUFFERS 3

#define SAMPLE_RATE 44100

#define FORMAT AL_FORMAT_MONO16

#define SAMPLE_TIME ((double)1/(double)SAMPLE_RATE)
#define BLOCK_TIME (BUFFER_SIZE*SAMPLE_TIME)

#define PD_NUM_INPUTS 0
#define PD_BLOCK_SIZE 64


#define NSEC_PER_SEC 1000000000



typedef struct {
  HsStablePtr pdChan;
  ALuint *allSourceIDs;
  int numSources;
  int bufferSize;
  short *tempBuffer;
  short *pdBuffer;
  int pdTicks;
  int threadSleepNsec;
} OpenALThreadData;

// Function prototypes
bool check_source_ready(ALuint sourceID);
ALuint create_source(int bufferSize, int numBuffers);
bool tick_source_stream(ALuint sourceID, int sourceNum, OpenALThreadData threadData);
ALuint* startAudio(HsStablePtr pdChan);
void *openal_thread_loop(void *threadArg);
void checkALError(void);

// Checks if an OpenAL source has finished processing any of its streaming buffers
bool check_source_ready(ALuint sourceID) {
  ALint numBuffersToFill;
  alGetSourcei(sourceID, AL_BUFFERS_PROCESSED, &numBuffersToFill);
  return numBuffersToFill > 0;
}

ALuint create_source(int bufferSize, int numBuffers) {
  int sourceBuffer[bufferSize] = {0};
  ALuint sourceID;
  ALuint sourceBufferIDs[numBuffers];

  // Create our buffers and source
  alGenBuffers(numBuffers, sourceBufferIDs);
  alGenSources(1, &sourceID);
  if(alGetError() != AL_NO_ERROR) {
    fprintf(stderr, "Error generating :(\n");
    return 1;
  }

  // Fill the buffers with initial data
  for (int i = 0; i < numBuffers; ++i) {
    alBufferData(sourceBufferIDs[i], FORMAT, sourceBuffer, bufferSize * sizeof(short), SAMPLE_RATE);
  }
  
  if(alGetError() != AL_NO_ERROR) {
    fprintf(stderr, "Error loading :(\n");
    return 1;
  }

  // Queue the initial buffers so we have something to dequeue and begin playing
  alSourceQueueBuffers(sourceID, numBuffers, sourceBufferIDs);
  alSourcePlay(sourceID);

  if(alGetError() != AL_NO_ERROR) {
    fprintf(stderr, "Error starting :(\n");
    return 1;
  }
  return sourceID;
}

bool tick_source_stream(ALuint sourceID, int sourceNum, OpenALThreadData threadData) {
  int numSources = threadData->numSources;
  int bufferSize = threadData->bufferSize;
  ALint numBuffersToFill;
  alGetSourcei(sourceID, AL_BUFFERS_PROCESSED, &numBuffersToFill);
  if(numBuffersToFill <= 0) {
    return false;
  }

  // printf("%i Getting %i buffers\n", sourceID, numBuffersToFill);
  
  // NOTE: I'm not paying attention to numBuffersToFill; we only fill one buffer each tick.

  // Copy interleaved output of pd into OpenAL buffer for this source
  // TODO: SIMD this
  for (int n = 0; n < bufferSize; ++n) {
    threadData->tempBuffer[n] = pdBuffer[sourceNum + (n * numSources)];
  }
  
  // sine_into_buffer(tempBuffer, bufferSize, freq, tOffset);
  ALuint bufferID;
  alSourceUnqueueBuffers(sourceID, 1, &bufferID);
  alBufferData(bufferID, FORMAT, threadData->tempBuffer, bufferSize * sizeof(short), SAMPLE_RATE);
  alSourceQueueBuffers(sourceID, 1, &bufferID);
  if(alGetError() != AL_NO_ERROR) {
    fprintf(stderr, "Error buffering :(\n");
    return false;
  }
  
  ALint isPlaying;
  alGetSourcei(sourceID, AL_SOURCE_STATE, &isPlaying);
  if(isPlaying != AL_PLAYING) {
    printf("Restarting play\n");
    alSourcePlay(sourceID);
  }
  return true;
}



ALuint* startAudio(HsStablePtr pdChan, int numSources, int bufferSize) {
  const int pdBufferSize = bufferSize * numSources;
  
  libpd_init();
  libpd_init_audio(PD_NUM_INPUTS, numSources, SAMPLE_RATE);

  libpd_start_message(1); // one entry in list
  libpd_add_float(1.0f);
  libpd_finish_message("pd", "dsp");

  // Open the default device and create an OpenAL context
  ALCdevice *device;
  ALCcontext *context;
  ALuint *allSourceIDs = (ALuint*)malloc(numSources * sizeof(ALuint));

  device = alcOpenDevice(NULL);
  if(!device) {
    fprintf(stderr, "Couldn't open OpenAL device :-(\n");
    return allSourceIDs;
  }
  context = alcCreateContext(device, NULL);
  alcMakeContextCurrent(context);
  if(!context) {
    fprintf(stderr, "Couldn't create OpenAL context :-(\n");
    return allSourceIDs;
  }

  #if defined(_WIN32)
  // TODO OpenAL-soft HRTF enable here
  #else
  static alcMacOSXRenderingQualityProcPtr alcMacOSXRenderingQuality = NULL;
  alcMacOSXRenderingQuality = (alcMacOSXRenderingQualityProcPtr) alcGetProcAddress(NULL, (const ALCchar*) "alcMacOSXRenderingQuality");
  alcMacOSXRenderingQuality(ALC_MAC_OSX_SPATIAL_RENDERING_QUALITY_HIGH);
  #endif

  
  for (int i = 0; i < numSources; ++i) {
    allSourceIDs[i] = create_source(bufferSize, numSources);
    printf("Created source with ID: %i\n", allSourceIDs[i]);
  }

  add_reverb(allSourceIDs, numSources);

  OpenALThreadData *threadData = (OpenALThreadData *)malloc(sizeof(OpenALThreadData));
  threadData->allSourceIDs    = allSourceIDs;
  threadData->pdChan          = pdChan;
  threadData->numSources      = numSources;
  threadData->bufferSize      = bufferSize;
  threadData->tempBuffer      = calloc(bufferSize * sizeof(short));
  threadData->pdBuffer        = calloc(pdBufferSize * sizeof(short));
  threadData->pdTicks         = bufferSize/PD_BLOCK_SIZE;
  threadData->threadSleepNsec = (bufferSize/SAMPLE_RATE) * NSEC_PER_SEC / 2;

  // Spread the sources out
  for (int i = 0; i < numSources; ++i) {
    float pan = ((float)i/(float)numSources) * 2 - 1;

    ALuint sourceID = allSourceIDs[i];
    
    ALfloat sourcePos[] = {pan,0,-1};
    alSourcefv(sourceID, AL_POSITION, sourcePos);
  }

  pthread_t thread;
  pthread_create(&thread, NULL, openal_thread_loop, (void *)threadData);

  return allSourceIDs;
}

void *openal_thread_loop(void *threadArg) {
  OpenALThreadData *threadData = (OpenALThreadData *)threadArg;
  const ALuint* allSourceIDs = threadData->allSourceIDs;
  const int numSources = threadData->numSources;

  while (1) {
    // We want to wait until *all* sources are ready for new data, 
    // not just one, so we can fill them all at once.

    // Continuously check each source to see if it has buffers ready to fill
    int numSourcesReady = 0;
    for (int i = 0; i < numSources; ++i) {
      ALuint sourceID = allSourceIDs[i];
      if (check_source_ready(sourceID)) {
        numSourcesReady++;
      }
    }
    // Once all have reported they're ready, fill them all.
    // We fill them here with varying enveloped sine frequencies.
    if (numSourcesReady == numSources) {

      // buffer size is num channels * num ticks * num samples per tick (libpd_blocksize(), default 64)
      // samples are interleaved, so 
      // l0 r0 f0 l1 r1 f1 l2 r2 f2
      // sample(n) = pdBuffer[sourceNum + (n * numSources)]

      // libpd_process_short(pdTicks, 0, pdBuffer);
      // Call into Haskell to ensure processing occurs 
      // on the dedicated thread we create for libpd
      processShort(threadData->pdChan, threadData->pdTicks, 0, pdBuffer);

      for (int i = 0; i < numSources; ++i) {
        
        ALuint sourceID = allSourceIDs[i];
        tick_source_stream(sourceID, i, threadData);
      
      }
    }
    nanosleep((struct timespec[]){{0, threadData->threadSleepNsec}}, NULL);
  }
}


// Wrap OpenAL API for easier use with Haskell's FFI
void setOpenALSourcePositionRaw(ALuint sourceID, ALfloat *values) {
  // printf("Source %i Position: (%f %f %f)\n", sourceID, values[0],values[1],values[2]);
  alSourcefv(sourceID, AL_POSITION, values);
  checkALError();
}

void setOpenALListenerPositionRaw(ALfloat *values) {
  // printf("Listener Position (note: we are inverting x in Haskell binding): (%f %f %f)\n", values[0],values[1],values[2]);
  alListenerfv(AL_POSITION, values);
}
void setOpenALListenerOrientationRaw(ALfloat *values) {
  // printf("Orientation UP: (%f %f %f) AT: (%f %f %f)\n", values[0],values[1],values[2],values[3],values[4],values[5]);
  alListenerfv(AL_ORIENTATION, values);
}

void checkALError(void) {
  
  ALenum error = alGetError();
  switch(error) {
    case AL_NO_ERROR:
      break;                              

    case AL_INVALID_NAME:
      printf("OpenAL Error: Invalid name\n");
      break;                          

    case AL_INVALID_ENUM:
      printf("OpenAL Error: Invalid enum\n");
      break;                          

    case AL_INVALID_VALUE:
      printf("OpenAL Error: Invalid value\n");
      break;                         

    case AL_INVALID_OPERATION:
      printf("OpenAL Error: Invalid operation\n");
      break;                     

    case AL_OUT_OF_MEMORY:
      printf("OpenAL Error: Out of memory\n");
      break;                         
  }
}
