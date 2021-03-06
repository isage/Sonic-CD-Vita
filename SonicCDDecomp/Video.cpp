﻿#include "RetroEngine.hpp"

int currentVideoFrame = 0;
int videoFrameCount = 0;
int videoWidth  = 0;
int videoHeight       = 0;

THEORAPLAY_Decoder *videoDecoder;
const THEORAPLAY_VideoFrame *videoVidData;
const THEORAPLAY_AudioPacket *videoAudioData;
THEORAPLAY_Io callbacks;

byte videoData = 0;
int videoFilePos = 0;
bool videoPlaying = 0;
int vidFrameMS = 0;
int vidBaseticks = 0;


bool videoSkipped = false;

static long videoRead(THEORAPLAY_Io *io, void *buf, long buflen)
{
    FileIO *file    = (FileIO *)io->userdata;
    const size_t br = fRead(buf, 1, buflen * sizeof(byte), file);
    if (br == 0)
        return -1;
    return (int)br;
} // IoFopenRead

static void videoClose(THEORAPLAY_Io *io)
{
    FileIO *file = (FileIO *)io->userdata;
    fClose(file);
}

void PlayVideoFile(char *filePath) { 
    char filepath[0x100];


    StrCopy(filepath, gamePath);
    StrAdd(filepath, "videos/");
    StrAdd(filepath, filePath);
    StrAdd(filepath, ".ogv");

    FileIO *file = fOpen(filepath, "rb");
    if (file) {
        printLog("Loaded File '%s'!", filepath);

        callbacks.read     = videoRead;
        callbacks.close    = videoClose;
        callbacks.userdata = (void *)file;
        videoDecoder       = THEORAPLAY_startDecode(&callbacks, /*FPS*/ 30, THEORAPLAY_VIDFMT_RGBA);

        if (!videoDecoder) {
            printLog("Video Decoder Error!");
            return;
        }
        while (!videoAudioData || !videoVidData) {
            if (!videoAudioData)
                videoAudioData = THEORAPLAY_getAudio(videoDecoder);
            if (!videoVidData)
                videoVidData = THEORAPLAY_getVideo(videoDecoder);
        }
        if (!videoAudioData || !videoVidData) {
            printLog("Video or Audio Error!");
            return;
        }

        //clear audio data, we dont use it
        while ((videoAudioData = THEORAPLAY_getAudio(videoDecoder)) != NULL) THEORAPLAY_freeAudio(videoAudioData);

        videoWidth  = videoVidData->width;
        videoHeight = videoVidData->height;
        SetupVideoBuffer(videoWidth, videoHeight);
        vidBaseticks = SDL_GetTicks();
        vidFrameMS     = (videoVidData->fps == 0.0) ? 0 : ((Uint32)(1000.0 / videoVidData->fps));
        videoPlaying = true;
        trackID        = TRACK_COUNT - 1;

        // "temp" but I really cannot be bothered to go through the nightmare that is streaming the audio data
        // (yes I tried, and probably cut years off my life)
        StrCopy(filepath, gamePath);
        StrAdd(filepath, "videos/");
        StrAdd(filepath, filePath);
        if (StrComp(filePath, "Good_Ending") || StrComp(filePath, "Bad_Ending") || StrComp(filePath, "Opening")) {
            if (!GetGlobalVariableByName("Options.Soundtrack"))
                StrAdd(filepath, "JP");
            else
                StrAdd(filepath, "US");
        }
        StrAdd(filepath, ".ogg");

        TrackInfo *track = &musicTracks[trackID];
        StrCopy(track->fileName, filepath);
        track->trackLoop = false;
        track->loopPoint = 0;

        //Switch it off so the reader can access it
        bool df              = Engine.usingDataFile;
        Engine.usingDataFile = false;
        PlayMusic(trackID);
        Engine.usingDataFile = df;

        videoSkipped = false;

        Engine.gameMode = ENGINE_VIDEOWAIT;
    }
    else {
        printLog("Couldn't find file '%s'!", filepath);
    }
    
}

void UpdateVideoFrame()
{
    if (videoPlaying) {
        if (videoFrameCount > currentVideoFrame) {
            GFXSurface *surface = &gfxSurface[videoData];
            int fileBuffer               = 0;
            FileRead(&fileBuffer, 1);
            videoFilePos += fileBuffer;
            FileRead(&fileBuffer, 1);
            videoFilePos += fileBuffer << 8;
            FileRead(&fileBuffer, 1);
            videoFilePos += fileBuffer << 16;
            FileRead(&fileBuffer, 1);
            videoFilePos += fileBuffer << 24;

            byte clr[3];
            for (int i = 0; i < 0x80; ++i) {
                FileRead(&clr, 3);
                activePalette32[i] = (0xFF<<24) | (clr[2] << 16) | (clr[1] << 8) | (clr[0]);
            }

            FileRead(&fileBuffer, 1);
            while (fileBuffer != ',') FileRead(&fileBuffer, 1); // gif image start identifier

            FileRead(&fileBuffer, 2); // IMAGE LEFT
            FileRead(&fileBuffer, 2); // IMAGE TOP
            FileRead(&fileBuffer, 2); // IMAGE WIDTH
            FileRead(&fileBuffer, 2); // IMAGE HEIGHT
            FileRead(&fileBuffer, 1); // PaletteType
            bool interlaced = (fileBuffer & 0x40) >> 6;
            if (fileBuffer >> 7 == 1) {
                int c = 0x80;
                do {
                    ++c;
                    FileRead(&fileBuffer, 3);
                } while (c != 0x100);
            }
            ReadGifPictureData(surface->width, surface->height, interlaced,
                                       graphicData, surface->dataPosition);

            SetFilePosition(videoFilePos);
            ++currentVideoFrame;
        }
        else {
            videoPlaying = 0;
            CloseFile();
        }
    }
}

int ProcessVideo()
{
    if (videoPlaying) {
        CheckKeyPress(&keyPress, 0x10);

        if (videoSkipped && fadeMode < 0xFF) {
            fadeMode += 8;
        }

        if (keyPress.A) {
            if (!videoSkipped) 
                fadeMode = 0;

            videoSkipped = true;
        }

        if (!THEORAPLAY_isDecoding(videoDecoder) || (videoSkipped && fadeMode >= 0xFF)) {
            if (videoSkipped && fadeMode >= 0xFF)
                fadeMode = 0;

            if (videoVidData)
                THEORAPLAY_freeVideo(videoVidData);
            if (videoAudioData)
                THEORAPLAY_freeAudio(videoAudioData);
            if (videoDecoder)
                THEORAPLAY_stopDecode(videoDecoder);

            CloseVideoBuffer();
            videoPlaying = false;

            return 1; // video finished
        }

        // Don't pause or it'll go wild
        if (videoPlaying) {
            const Uint32 now = (SDL_GetTicks() - vidBaseticks);

            if (!videoVidData)
                videoVidData = THEORAPLAY_getVideo(videoDecoder);

            // Play video frames when it's time.
            if (videoVidData && (videoVidData->playms <= now)) {
                if (vidFrameMS && ((now - videoVidData->playms) >= vidFrameMS)) {

                    // Skip frames to catch up, but keep track of the last one+
                    //  in case we catch up to a series of dupe frames, which
                    //  means we'd have to draw that final frame and then wait for
                    //  more.

                    const THEORAPLAY_VideoFrame *last = videoVidData;
                    while ((videoVidData = THEORAPLAY_getVideo(videoDecoder)) != NULL) {
                        THEORAPLAY_freeVideo(last);
                        last = videoVidData;
                        if ((now - videoVidData->playms) < vidFrameMS)
                            break;
                    }

                    if (!videoVidData)
                        videoVidData = last;
                }

                // do nothing; we're far behind and out of options.
                if (!videoVidData) {
                    // video lagging uh oh
                }

                uint px = 0;
                int pitch = 0;
                uint* pixels = NULL;
                SDL_LockTexture(Engine.videoBuffer, NULL, (void **)&pixels, &pitch);

                for (uint i = 0; i < (videoWidth * videoHeight) * sizeof(uint); i += sizeof(uint)) {
                    pixels[px++] = (0xFF << 24 | videoVidData->pixels[i+2] << 16
                                                     | videoVidData->pixels[i + 1] << 8 | videoVidData->pixels[i] << 0);


                }
                SDL_UnlockTexture(Engine.videoBuffer);


                THEORAPLAY_freeVideo(videoVidData);
                videoVidData = NULL;
            }

            //Clear audio data
            while ((videoAudioData = THEORAPLAY_getAudio(videoDecoder)) != NULL) THEORAPLAY_freeAudio(videoAudioData);

            return 2; // its playing as expected
        }
    }

    return 0; // its not even initialised
}


void SetupVideoBuffer(int width, int height) {
    Engine.videoBuffer = SDL_CreateTexture(Engine.renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, width, height);

    if (!Engine.videoBuffer) 
        printLog("Failed to create video buffer!");
}
void CloseVideoBuffer() {
    if (videoPlaying) {
        SDL_DestroyTexture(Engine.videoBuffer);
        Engine.videoBuffer = nullptr;
    }
}