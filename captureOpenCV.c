/*
 * Copyright (c) 2012-2013, NVIDIA CORPORATION. All rights reserved.
 * All information contained herein is proprietary and confidential to NVIDIA
 * Corporation.  Any use, reproduction, or disclosure without the written
 * permission of NVIDIA Corporation is prohibited.
 */

// edited by Hyundai Autron
// gcc -I. -I./utils `pkg-config opencv --cflags` -I./include  -c -o captureOpenCV.o captureOpenCV.c
// gcc -I. -I./utils `pkg-config opencv --cflags` -I./include  -c -o nvthread.o nvthread.c
// gcc  -o captureOpenCV captureOpenCV.o nvthread.o  -L ./utils -lnvmedia -lnvtestutil_board -lnvtestutil_capture_input -lnvtestutil_i2c -lpthread `pkg-config opencv --libs`
// last update
// dh test

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/time.h>

#include <nvcommon.h>
#include <nvmedia.h>

#include <testutil_board.h>
#include <testutil_capture_input.h>

#include "nvthread.h"

#include <highgui.h>
#include <cv.h>
#include <ResTable_720To320.h>
#include <pthread.h>
#include <unistd.h>     // for sleep

#define VIP_BUFFER_SIZE 6
#define VIP_FRAME_TIMEOUT_MS 100
#define VIP_NAME "vip"

#define MESSAGE_PRINTF printf

#define CRC32_POLYNOMIAL 0xEDB88320L

#define RESIZE_WIDTH  320
#define RESIZE_HEIGHT 240

static NvMediaVideoSurface *capSurf = NULL;

pthread_cond_t      cond  = PTHREAD_COND_INITIALIZER;
pthread_mutex_t     mutex = PTHREAD_MUTEX_INITIALIZER;

int table_298[256];
int table_409[256];
int table_100[256];
int table_208[256];
int table_516[256];

typedef struct
{
    I2cId i2cDevice;
  
    CaptureInputDeviceId vipDeviceInUse;
    NvMediaVideoCaptureInterfaceFormat vipInputtVideoStd;
    unsigned int vipInputWidth;
    unsigned int vipInputHeight;
    float vipAspectRatio;

    unsigned int vipMixerWidth;
    unsigned int vipMixerHeight;

    NvBool vipDisplayEnabled;
    NvMediaVideoOutputType vipOutputType;
    NvMediaVideoOutputDevice vipOutputDevice[2];
    NvBool vipFileDumpEnabled;
    char * vipOutputFileName;

    unsigned int vipCaptureTime;
    unsigned int vipCaptureCount;
} TestArgs;

typedef struct
{
    NvMediaVideoSurface *surf;
    NvBool last;
} QueueElem;

typedef struct
{
    char *name;

    NvSemaphore *semStart, *semDone;

    NvMediaVideoCapture *capture;
    NvMediaVideoMixer *mixer;
    FILE *fout;

    unsigned int inputWidth;
    unsigned int inputHeight;

    unsigned int timeout;

    NvBool displayEnabled;
    NvBool fileDumpEnabled;

    NvBool timeNotCount;
    unsigned int last;
} CaptureContext;

static NvBool stop = NVMEDIA_FALSE;

static void SignalHandler(int signal)
{
    stop = NVMEDIA_TRUE;
    MESSAGE_PRINTF("%d signal received\n", signal);
}

static void GetTime(NvMediaTime *time)
{
    struct timeval t;

    gettimeofday(&t, NULL);

    time->tv_sec = t.tv_sec;
    time->tv_nsec = t.tv_usec * 1000;
}

static void AddTime(NvMediaTime *time, NvU64 uSec, NvMediaTime *res)
{
    NvU64 t, newTime;

    t = (NvU64)time->tv_sec * 1000000000LL + (NvU64)time->tv_nsec;
    newTime = t + uSec * 1000LL;
    res->tv_sec = newTime / 1000000000LL;
    res->tv_nsec = newTime % 1000000000LL;
}

//static NvS64 SubTime(NvMediaTime *time1, NvMediaTime *time2)
static NvBool SubTime(NvMediaTime *time1, NvMediaTime *time2)
{
    NvS64 t1, t2, delta;

    t1 = (NvS64)time1->tv_sec * 1000000000LL + (NvS64)time1->tv_nsec;
    t2 = (NvS64)time2->tv_sec * 1000000000LL + (NvS64)time2->tv_nsec;
    delta = t1 - t2;

//    return delta / 1000LL;
    return delta > 0LL;
}


static void DisplayUsage(void)
{
    printf("Usage : nvmedia_capture [options]\n");
    printf("Brief: Displays this help if no arguments are given. Engages the respective capture module whenever a single \'c\' or \'v\' argument is supplied using default values for the missing parameters.\n");
    printf("Options:\n");
    printf("-va <aspect ratio>    VIP aspect ratio (default = 1.78 (16:9))\n");
    printf("-vmr <width>x<height> VIP mixer resolution (default 800x480)\n");
    printf("-vf <file name>       VIP output file name; default = off\n");
    printf("-vt [seconds]         VIP capture duration (default = 10 secs); overridden by -vn; default = off\n");
    printf("-vn [frames]          # VIP frames to be captured (default = 300); default = on if -vt is not used\n");
}

static int ParseOptions(int argc, char *argv[], TestArgs *args)
{
    int i = 1;

    // Set defaults if necessary - TBD
    args->i2cDevice = I2C4;     // i2c chnnel

    args->vipDeviceInUse = AnalogDevices_ADV7182;
    args->vipInputtVideoStd = NVMEDIA_VIDEO_CAPTURE_INTERFACE_FORMAT_VIP_NTSC;
    args->vipInputWidth = 720;
    args->vipInputHeight = 480;
    args->vipAspectRatio = 0.0f;

    args->vipMixerWidth = 800;
    args->vipMixerHeight = 480;

    args->vipDisplayEnabled = NVMEDIA_FALSE;
    args->vipOutputType = NvMediaVideoOutputType_OverlayYUV;
    args->vipOutputDevice[0] = NvMediaVideoOutputDevice_LVDS;
    args->vipFileDumpEnabled = NVMEDIA_FALSE;
    args->vipOutputFileName = NULL;

    args->vipCaptureTime = 0;
    args->vipCaptureCount = 0;

  

    if(i < argc && argv[i][0] == '-')
    {
        while(i < argc && argv[i][0] == '-')
        {
            if(i > 1 && argv[i][1] == '-')
            {
                MESSAGE_PRINTF("Using basic and custom options together is not supported\n");
                return 0;
            }

            // Get options
            if(!strcmp(argv[i], "-va"))
            {
                if(++i < argc)
                {
                    if(sscanf(argv[i], "%f", &args->vipAspectRatio) != 1 || args->vipAspectRatio <= 0.0f) // TBC
                    {
                        MESSAGE_PRINTF("Bad VIP aspect ratio: %s\n", argv[i]);
                        return 0;
                    }
                }
                else
                {
                    MESSAGE_PRINTF("Missing VIP aspect ratio\n");
                    return 0;
                }
            }
            else if(!strcmp(argv[i], "-vmr"))
            {
                if(++i < argc)
                {
                    if(sscanf(argv[i], "%ux%u", &args->vipMixerWidth, &args->vipMixerHeight) != 2)
                    {
                        MESSAGE_PRINTF("Bad VIP mixer resolution: %s\n", argv[i]);
                        return 0;
                    }
                }
                else
                {
                    MESSAGE_PRINTF("Missing VIP mixer resolution\n");
                    return 0;
                }
            }
            else if(!strcmp(argv[i], "-vf"))
            {
                args->vipFileDumpEnabled = NVMEDIA_TRUE;
                if(++i < argc)
                    args->vipOutputFileName = argv[i];
                else
                {
                    MESSAGE_PRINTF("Missing VIP output file name\n");
                    return 0;
                }
            }
            else if(!strcmp(argv[i], "-vt"))
            {
                if(++i < argc)
                    if(sscanf(argv[i], "%u", &args->vipCaptureTime) != 1)
                    {
                        MESSAGE_PRINTF("Bad VIP capture duration: %s\n", argv[i]);
                        return 0;
                    }
            }
            else if(!strcmp(argv[i], "-vn"))
            {
                if(++i < argc)
                    if(sscanf(argv[i], "%u", &args->vipCaptureCount) != 1)
                    {
                        MESSAGE_PRINTF("Bad VIP capture count: %s\n", argv[i]);
                        return 0;
                    }
            }
            else
            {
                MESSAGE_PRINTF("%s is not a supported option\n", argv[i]);
                return 0;
            }

            i++;
        }
    }

    if(i < argc)
    {
        MESSAGE_PRINTF("%s is not a supported option\n", argv[i]);
        return 0;
    }

    // Check for consistency
    if(i < 2)
    {
        DisplayUsage();
        return 0;
    }


    if(args->vipAspectRatio == 0.0f)
        args->vipAspectRatio = 1.78f;

    if(!args->vipDisplayEnabled && !args->vipFileDumpEnabled)
        args->vipDisplayEnabled = NVMEDIA_TRUE;


    if(!args->vipCaptureTime && !args->vipCaptureCount)
        args->vipCaptureCount = 300;
    else if(args->vipCaptureTime && args->vipCaptureCount)
        args->vipCaptureTime = 0;



    return 1;
}

static int DumpFrame(FILE *fout, NvMediaVideoSurface *surf)
{
    NvMediaVideoSurfaceMap surfMap;
    unsigned int width, height;

    if(NvMediaVideoSurfaceLock(surf, &surfMap) != NVMEDIA_STATUS_OK)
    {
        MESSAGE_PRINTF("NvMediaVideoSurfaceLock() failed in DumpFrame()\n");
        return 0;
    }

    width = surf->width;
    height = surf->height;

    unsigned char *pY[2] = {surfMap.pY, surfMap.pY2};
    unsigned char *pU[2] = {surfMap.pU, surfMap.pU2};
    unsigned char *pV[2] = {surfMap.pV, surfMap.pV2};
    unsigned int pitchY[2] = {surfMap.pitchY, surfMap.pitchY2};
    unsigned int pitchU[2] = {surfMap.pitchU, surfMap.pitchU2};
    unsigned int pitchV[2] = {surfMap.pitchV, surfMap.pitchV2};
    unsigned int i, j;

    for(i = 0; i < 2; i++)
    {
        for(j = 0; j < height / 2; j++)
        {
            fwrite(pY[i], width, 1, fout);
            pY[i] += pitchY[i];
        }
        for(j = 0; j < height / 2; j++)
        {
            fwrite(pU[i], width / 2, 1, fout);
            pU[i] += pitchU[i];
        }
        for(j = 0; j < height / 2; j++)
        {
           fwrite(pV[i], width / 2, 1, fout);
           pV[i] += pitchV[i];
        }
    }


    NvMediaVideoSurfaceUnlock(surf);

    return 1;
}

static int Frame2Ipl(IplImage* img)
{
    NvMediaVideoSurfaceMap surfMap;
    unsigned int resWidth, resHeight;
    char r,g,b;
    unsigned char y,u,v;
    int num;

    if(NvMediaVideoSurfaceLock(capSurf, &surfMap) != NVMEDIA_STATUS_OK)
    {
        MESSAGE_PRINTF("NvMediaVideoSurfaceLock() failed in Frame2Ipl()\n");
        return 0;
    }

    unsigned char *pY[2] = {surfMap.pY, surfMap.pY2};
    unsigned char *pU[2] = {surfMap.pU, surfMap.pU2};
    unsigned char *pV[2] = {surfMap.pV, surfMap.pV2};
    unsigned int pitchY[2] = {surfMap.pitchY, surfMap.pitchY2};
    unsigned int pitchU[2] = {surfMap.pitchU, surfMap.pitchU2};
    unsigned int pitchV[2] = {surfMap.pitchV, surfMap.pitchV2};
    unsigned int i, j, k, x;
    unsigned int stepY, stepU, stepV;
    
    resWidth = RESIZE_WIDTH;
    resHeight = RESIZE_HEIGHT;
    
    // Frame2Ipl
    img->nSize = 112;
    img->ID = 0;
    img->nChannels = 3;
    img->alphaChannel = 0;
    img->depth = IPL_DEPTH_8U;    // 8
    img->colorModel[0] = 'R';
    img->colorModel[1] = 'G';
    img->colorModel[2] = 'B';
    img->channelSeq[0] = 'B';
    img->channelSeq[1] = 'G';
    img->channelSeq[2] = 'R';
    img->dataOrder = 0;
    img->origin = 0;
    img->align = 4;
    img->width = resWidth;
    img->height = resHeight;
    img->imageSize = resHeight*resWidth*3;
    img->widthStep = resWidth*3;
    img->BorderMode[0] = 0;
    img->BorderMode[1] = 0;
    img->BorderMode[2] = 0;
    img->BorderMode[3] = 0;
    img->BorderConst[0] = 0;
    img->BorderConst[1] = 0;
    img->BorderConst[2] = 0;
    img->BorderConst[3] = 0;
    
    stepY = 0;
    stepU = 0;
    stepV = 0;
    i = 0;
    
    for(j = 0; j < resHeight; j++)
    {
        for(k = 0; k < resWidth; k++)
        {
            x = ResTableX_720To320[k];
            y = pY[i][stepY+x];
            u = pU[i][stepU+x/2];
            v = pV[i][stepV+x/2];
            
            // YUV to RGB (fast but somewhat inaccurate)
            //r =  ( table_298[y] + table_409[v] ) >> 8;
            //g =  ( table_298[y] - table_100[u] - table_208[v] ) >> 8;
            //b =  ( table_298[y] + table_516[u] ) >> 8;
            //r =  ( 298*(y-16) + 409*(v-128) + 128 ) >> 8;
            //g =  ( 298*(y-16) - 100*(u-128) - 208*(v-128) + 128 ) >> 8;
            //b =  ( 298*(y-16) + 516*(u-128) + 128 ) >> 8;

            // YUV to RGB (accurate but slow)
            r = 1.164*(y-16) + 1.596*(v-128); 
            g = 1.164*(y-16) - 0.813*(v-128) - 0.391*(u-128); 
            b = 1.164*(y-16) + 2.018*(u-128);  


            num = 3*k+3*resWidth*(j);
            img->imageData[num] = b;
            img->imageData[num+1] = g;
            img->imageData[num+2] = r;
            //img->imageDataOrigin[num] = b;
            //img->imageDataOrigin[num+1] = g;
            //img->imageDataOrigin[num+2] = r;
        }
        stepY += pitchY[i];
        stepU += pitchU[i];
        stepV += pitchV[i];
    }

    
    NvMediaVideoSurfaceUnlock(capSurf);

    return 1;
}

static unsigned int CaptureThread(void *params)
{
    int i = 0;
    NvU64 stime, ctime;
    NvMediaTime t1 = {0}, t2 = {0}, st = {0}, ct = {0};
    CaptureContext *ctx = (CaptureContext *)params;
    NvMediaVideoSurface *releaseList[4] = {NULL}, **relList;
    NvMediaRect primarySrcRect;
    NvMediaPrimaryVideo primaryVideo;

    primarySrcRect.x0 = 0;
    primarySrcRect.y0 = 0;
    primarySrcRect.x1 = ctx->inputWidth;
    primarySrcRect.y1 = ctx->inputHeight;

    primaryVideo.next = NULL;
    primaryVideo.previous = NULL;
    primaryVideo.previous2 = NULL;
    primaryVideo.srcRect = &primarySrcRect;
    primaryVideo.dstRect = NULL;
    

    NvSemaphoreDecrement(ctx->semStart, NV_TIMEOUT_INFINITE);

    if(ctx->timeNotCount)
    {
        GetTime(&t1);
        AddTime(&t1, ctx->last * 1000000LL, &t1);
        GetTime(&t2);
        printf("timeNotCount\n");
    }
    GetTime(&st);
    stime = (NvU64)st.tv_sec * 1000000000LL + (NvU64)st.tv_nsec;

    while((ctx->timeNotCount? (SubTime(&t1, &t2)): ((unsigned int)i < ctx->last)) && !stop)
    {
        GetTime(&ct);
        ctime = (NvU64)ct.tv_sec * 1000000000LL + (NvU64)ct.tv_nsec;
        printf("frame=%3d, time=%llu.%09llu[s] \n", i, (ctime-stime)/1000000000LL, (ctime-stime)%1000000000LL);

        pthread_mutex_lock(&mutex);            // for ControlThread()
        
        if(!(capSurf = NvMediaVideoCaptureGetFrame(ctx->capture, ctx->timeout)))
        { // TBD
            MESSAGE_PRINTF("NvMediaVideoCaptureGetFrame() failed in %sThread\n", ctx->name);
            stop = NVMEDIA_TRUE;
            break;
        }
      
        if(i%3 == 0)                        // once in three loop = 10 Hz
            pthread_cond_signal(&cond);        // ControlThread() is called

        pthread_mutex_unlock(&mutex);        // for ControlThread()
        
        primaryVideo.current = capSurf;
        primaryVideo.pictureStructure = NVMEDIA_PICTURE_STRUCTURE_TOP_FIELD;

        if(NVMEDIA_STATUS_OK != NvMediaVideoMixerRender(ctx->mixer, // mixer
                                                        NVMEDIA_OUTPUT_DEVICE_0, // outputDeviceMask
                                                        NULL, // background
                                                        &primaryVideo, // primaryVideo
                                                        NULL, // secondaryVideo
                                                        NULL, // graphics0
                                                        NULL, // graphics1
                                                        releaseList, // releaseList
                                                        NULL)) // timeStamp
        { // TBD
            MESSAGE_PRINTF("NvMediaVideoMixerRender() failed for the top field in %sThread\n", ctx->name);
            stop = NVMEDIA_TRUE;
        }

        primaryVideo.pictureStructure = NVMEDIA_PICTURE_STRUCTURE_BOTTOM_FIELD;
        if(NVMEDIA_STATUS_OK != NvMediaVideoMixerRender(ctx->mixer, // mixer
                                                        NVMEDIA_OUTPUT_DEVICE_0, // outputDeviceMask
                                                        NULL, // background
                                                        &primaryVideo, // primaryVideo
                                                        NULL, // secondaryVideo
                                                        NULL, // graphics0
                                                        NULL, // graphics1
                                                        releaseList, // releaseList
                                                        NULL)) // timeStamp
        { // TBD
            MESSAGE_PRINTF("NvMediaVideoMixerRender() failed for the bottom field in %sThread\n", ctx->name);
            stop = NVMEDIA_TRUE;
        }

        if(ctx->fileDumpEnabled)
        {
            if(!DumpFrame(ctx->fout, capSurf))
            { // TBD
                MESSAGE_PRINTF("DumpFrame() failed in %sThread\n", ctx->name);
                stop = NVMEDIA_TRUE;
            }

            if(!ctx->displayEnabled)
                releaseList[0] = capSurf;
        }
        
        relList = &releaseList[0];

        while(*relList)
        {
            if(NvMediaVideoCaptureReturnFrame(ctx->capture, *relList) != NVMEDIA_STATUS_OK)
            { // TBD
                MESSAGE_PRINTF("NvMediaVideoCaptureReturnFrame() failed in %sThread\n", ctx->name);
                stop = NVMEDIA_TRUE;
                break;
            }
            relList++;
        }

        if(ctx->timeNotCount)
            GetTime(&t2);

        i++;
    } // while end

    // Release any left-over frames
//    if(ctx->displayEnabled && capSurf && capSurf->type != NvMediaSurfaceType_YV16x2) // To allow returning frames after breaking out of the while loop in case of error
    if(ctx->displayEnabled && capSurf)
    {
        NvMediaVideoMixerRender(ctx->mixer, // mixer
                                NVMEDIA_OUTPUT_DEVICE_0, // outputDeviceMask
                                NULL, // background
                                NULL, // primaryVideo
                                NULL, // secondaryVideo
                                NULL, // graphics0
                                NULL, // graphics1
                                releaseList, // releaseList
                                NULL); // timeStamp

        relList = &releaseList[0];

        while(*relList)
        {
            if(NvMediaVideoCaptureReturnFrame(ctx->capture, *relList) != NVMEDIA_STATUS_OK)
                MESSAGE_PRINTF("NvMediaVideoCaptureReturnFrame() failed in %sThread\n", ctx->name);

            relList++;
        }
    }

    NvSemaphoreIncrement(ctx->semDone);
    return 0;
}

static void CheckDisplayDevice(NvMediaVideoOutputDevice deviceType, NvMediaBool *enabled, unsigned int *displayId)
{
    int outputDevices;
    NvMediaVideoOutputDeviceParams *outputParams;
    int i;

    // By default set it as not enabled (initialized)
    *enabled = NVMEDIA_FALSE;
    *displayId = 0;

    // Get the number of devices
    if(NvMediaVideoOutputDevicesQuery(&outputDevices, NULL) != NVMEDIA_STATUS_OK) {
        return;
    }

    // Allocate memory for information for all devices
    outputParams = malloc(outputDevices * sizeof(NvMediaVideoOutputDeviceParams));
    if(!outputParams) {
        return;
    }

    // Get device information for acll devices
    if(NvMediaVideoOutputDevicesQuery(&outputDevices, outputParams) != NVMEDIA_STATUS_OK) {
        free(outputParams);
        return;
    }

    // Find desired device
    for(i = 0; i < outputDevices; i++) {
        if((outputParams + i)->outputDevice == deviceType) {
            // Return information
            *enabled = (outputParams + i)->enabled;
            *displayId = (outputParams + i)->displayId;
            break;
        }
    }

    // Free information memory
    free(outputParams);
}

static void YUV2RGVtableInit(void)
{
    int i;
    
    for( i = 0 ; i < 256 ; i++ )
    {
        table_298[i] = 298*(i-16) + 128;
        table_409[i] = 409*(i-128);
        table_100[i] = 100*(i-128);
        table_208[i] = 208*(i-128);
        table_516[i] = 516*(i-128);
    }

}

void *ControlThread(void *unused)
{
    int i=0;
    char fileName[30];
    NvMediaTime pt1 ={0}, pt2 = {0};
    NvU64 ptime1, ptime2;
    struct timespec;
 
    IplImage* imgOrigin;
    IplImage* imgCanny;
    
    // cvCreateImage
    imgOrigin = cvCreateImage(cvSize(RESIZE_WIDTH, RESIZE_HEIGHT), IPL_DEPTH_8U, 3);
    imgCanny = cvCreateImage(cvGetSize(imgOrigin), IPL_DEPTH_8U, 1);
 
    while(1)
    {
        pthread_mutex_lock(&mutex);
        pthread_cond_wait(&cond, &mutex);
        
        
        GetTime(&pt1);
        ptime1 = (NvU64)pt1.tv_sec * 1000000000LL + (NvU64)pt1.tv_nsec;

        
        Frame2Ipl(imgOrigin); // save image to IplImage structure & resize image from 720x480 to 320x240
        pthread_mutex_unlock(&mutex);     
        
           
        cvCanny(imgOrigin, imgCanny, 100, 100, 3);
        
        sprintf(fileName, "captureImage/imgCanny%d.png", i);
        cvSaveImage(fileName , imgCanny, 0); 
        
        //sprintf(fileName, "captureImage/imgOrigin%d.png", i);
        //cvSaveImage(fileName, imgOrigin, 0);
        
        
        // TODO : control steering angle based on captured image ---------------
        
        
        
        // ---------------------------------------------------------------------
            
        GetTime(&pt2);
        ptime2 = (NvU64)pt2.tv_sec * 1000000000LL + (NvU64)pt2.tv_nsec;
        printf("--------------------------------operation time=%llu.%09llu[s]\n", (ptime2-ptime1)/1000000000LL, (ptime2-ptime1)%1000000000LL);  
        
         
        i++;
    }
}

int main(int argc, char *argv[])
{
    int err = -1;
    TestArgs testArgs;

    CaptureInputHandle handle;

    NvMediaVideoCapture *vipCapture = NULL;
    NvMediaDevice *device = NULL;
    NvMediaVideoMixer *vipMixer = NULL;
    NvMediaVideoOutput *vipOutput[2] = {NULL, NULL};
    NvMediaVideoOutput *nullOutputList[1] = {NULL};
    FILE *vipFile = NULL;

    NvSemaphore *vipStartSem = NULL, *vipDoneSem = NULL;
    NvThread *vipThread = NULL;

    CaptureContext vipCtx;
    NvMediaBool deviceEnabled = NVMEDIA_FALSE;
    unsigned int displayId;
    
    pthread_t cntThread;
    
    signal(SIGINT, SignalHandler);

    memset(&testArgs, 0, sizeof(TestArgs));
    if(!ParseOptions(argc, argv, &testArgs))
        return -1;

    printf("1. Create NvMedia capture \n");
    // Create NvMedia capture(s)
    switch (testArgs.vipDeviceInUse)
    {
        case AnalogDevices_ADV7180:
            break;
        case AnalogDevices_ADV7182:
        {
            CaptureInputConfigParams params;

            params.width = testArgs.vipInputWidth;
            params.height = testArgs.vipInputHeight;
            params.vip.std = testArgs.vipInputtVideoStd;

            if(testutil_capture_input_open(testArgs.i2cDevice, testArgs.vipDeviceInUse, NVMEDIA_TRUE, &handle) < 0)
            {
                MESSAGE_PRINTF("Failed to open VIP device\n");
                goto fail;
            }

            if(testutil_capture_input_configure(handle, &params) < 0)
            {
                MESSAGE_PRINTF("Failed to configure VIP device\n");
                goto fail;
            }

            break;
        }
        default:
            MESSAGE_PRINTF("Bad VIP device\n");
            goto fail;
    }
    

    if(!(vipCapture = NvMediaVideoCaptureCreate(testArgs.vipInputtVideoStd, // interfaceFormat
                                                NULL, // settings
                                                VIP_BUFFER_SIZE)))// numBuffers
    {
        MESSAGE_PRINTF("NvMediaVideoCaptureCreate() failed for vipCapture\n");
        goto fail;
    }
  

    printf("2. Create NvMedia device \n");
    // Create NvMedia device
    if(!(device = NvMediaDeviceCreate()))
    {
        MESSAGE_PRINTF("NvMediaDeviceCreate() failed\n");
        goto fail;
    }

    printf("3. Create NvMedia mixer(s) and output(s) and bind them \n");
    // Create NvMedia mixer(s) and output(s) and bind them
    unsigned int features = 0;


    features |= NVMEDIA_VIDEO_MIXER_FEATURE_VIDEO_SURFACE_TYPE_YV16X2;
    features |= NVMEDIA_VIDEO_MIXER_FEATURE_PRIMARY_VIDEO_DEINTERLACING; // Bob the 16x2 format by default
    if(testArgs.vipOutputType != NvMediaVideoOutputType_OverlayYUV)
        features |= NVMEDIA_VIDEO_MIXER_FEATURE_DVD_MIXING_MODE;

    if(!(vipMixer = NvMediaVideoMixerCreate(device, // device
                                            testArgs.vipMixerWidth, // mixerWidth
                                            testArgs.vipMixerHeight, // mixerHeight
                                            testArgs.vipAspectRatio, //sourceAspectRatio
                                            testArgs.vipInputWidth, // primaryVideoWidth
                                            testArgs.vipInputHeight, // primaryVideoHeight
                                            0, // secondaryVideoWidth
                                            0, // secondaryVideoHeight
                                            0, // graphics0Width
                                            0, // graphics0Height
                                            0, // graphics1Width
                                            0, // graphics1Height
                                            features , // features
                                            nullOutputList))) // outputList
    {
        MESSAGE_PRINTF("NvMediaVideoMixerCreate() failed for vipMixer\n");
        goto fail;
    }

    printf("4. Check that the device is enabled (initialized) \n");
    // Check that the device is enabled (initialized)
    CheckDisplayDevice(
        testArgs.vipOutputDevice[0],
        &deviceEnabled,
        &displayId);

    if((vipOutput[0] = NvMediaVideoOutputCreate(testArgs.vipOutputType, // outputType
                                                testArgs.vipOutputDevice[0], // outputDevice
                                                NULL, // outputPreference
                                                deviceEnabled, // alreadyCreated
                                                displayId, // displayId
                                                NULL))) // displayHandle
    {
        if(NvMediaVideoMixerBindOutput(vipMixer, vipOutput[0], NVMEDIA_OUTPUT_DEVICE_0) != NVMEDIA_STATUS_OK)
        {
            MESSAGE_PRINTF("Failed to bind VIP output to mixer\n");
            goto fail;
        }
    }
    else
    {
        MESSAGE_PRINTF("NvMediaVideoOutputCreate() failed for vipOutput\n");
        goto fail;
    }

 

    printf("5. Open output file(s) \n");
    // Open output file(s)
    if(testArgs.vipFileDumpEnabled)
    {
        vipFile = fopen(testArgs.vipOutputFileName, "w");
        if(!vipFile || ferror(vipFile))
        {
            MESSAGE_PRINTF("Error opening output file for VIP\n");
            goto fail;
        }
    }

    printf("6. Create vip pool(s), queue(s), fetch threads and stream start/done semaphores \n");
    // Create vip pool(s), queue(s), fetch threads and stream start/done semaphores
    if(NvSemaphoreCreate(&vipStartSem, 0, 1) != RESULT_OK)
    {
        MESSAGE_PRINTF("NvSemaphoreCreate() failed for vipStartSem\n");
        goto fail;
    }

    if(NvSemaphoreCreate(&vipDoneSem, 0, 1) != RESULT_OK)
    {
        MESSAGE_PRINTF("NvSemaphoreCreate() failed for vipDoneSem\n");
        goto fail;
    }

    vipCtx.name = VIP_NAME;

    vipCtx.semStart = vipStartSem;
    vipCtx.semDone = vipDoneSem;

    vipCtx.capture = vipCapture;
    vipCtx.mixer = vipMixer;
    vipCtx.fout = vipFile;

    vipCtx.inputWidth = testArgs.vipInputWidth;
    vipCtx.inputHeight = testArgs.vipInputHeight;

    vipCtx.timeout = VIP_FRAME_TIMEOUT_MS;

    vipCtx.displayEnabled = testArgs.vipDisplayEnabled;
    vipCtx.fileDumpEnabled = testArgs.vipFileDumpEnabled;

    if(testArgs.vipCaptureTime)
    {
        vipCtx.timeNotCount = NVMEDIA_TRUE;
        vipCtx.last = testArgs.vipCaptureTime;
    }
    else
    {
        vipCtx.timeNotCount = NVMEDIA_FALSE;
        vipCtx.last = testArgs.vipCaptureCount;
    }


    if(NvThreadCreate(&vipThread, CaptureThread, &vipCtx, NV_THREAD_PRIORITY_NORMAL) != RESULT_OK)
    {
        MESSAGE_PRINTF("NvThreadCreate() failed for vipThread\n");
        goto fail;
    }

    printf("wait for ADV7182 ... one second\n");
    sleep(1);
    
    printf("7. Kickoff \n");
    // Kickoff
    NvMediaVideoCaptureStart(vipCapture);
    NvSemaphoreIncrement(vipStartSem);
    
    printf("8. Control Thread\n");
    YUV2RGVtableInit();
    pthread_create(&cntThread, NULL, &ControlThread, NULL); 

    printf("9. Wait for completion \n");
    // Wait for completion
    NvSemaphoreDecrement(vipDoneSem, NV_TIMEOUT_INFINITE);


    err = 0;

fail: // Run down sequence
    // Destroy vip threads and stream start/done semaphores
    if(vipThread)
        NvThreadDestroy(vipThread);
    if(vipDoneSem)
        NvSemaphoreDestroy(vipDoneSem);
    if(vipStartSem)
        NvSemaphoreDestroy(vipStartSem);

    printf("10. Close output file(s) \n");
    // Close output file(s)
    if(vipFile)
        fclose(vipFile);
        
    // Unbind NvMedia mixer(s) and output(s) and destroy them
    if(vipOutput[0])
    {
        NvMediaVideoMixerUnbindOutput(vipMixer, vipOutput[0], NULL);
        NvMediaVideoOutputDestroy(vipOutput[0]);
    }
    if(vipOutput[1])
    {
        NvMediaVideoMixerUnbindOutput(vipMixer, vipOutput[1], NULL);
        NvMediaVideoOutputDestroy(vipOutput[1]);
    }
    if(vipMixer)
        NvMediaVideoMixerDestroy(vipMixer);


    // Destroy NvMedia device
    if(device)
        NvMediaDeviceDestroy(device);

    // Destroy NvMedia capture(s)
    if(vipCapture)
    {
        NvMediaVideoCaptureDestroy(vipCapture);

        // Reset VIP settings of the board
        switch (testArgs.vipDeviceInUse)
        {
            case AnalogDevices_ADV7180: // TBD
                break;
            case AnalogDevices_ADV7182: // TBD
                //testutil_capture_input_close(handle);
                break;
            default:
                break;
        }
    }
    
    return err;
}

