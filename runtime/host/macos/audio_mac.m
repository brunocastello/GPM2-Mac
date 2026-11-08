/* macOS audio output: a default-output AudioUnit (kAudioUnitSubType_DefaultOutput) pulls
 * `audio_render` (core mixer, 44100 Hz/16-bit/stereo) from the audio thread. The core never calls the guest there. */
#import <AudioToolbox/AudioToolbox.h>
#import <CoreAudio/CoreAudio.h>

#include "../host.h"

#include <stdio.h>
#include <dlfcn.h>

/* On Mac OS X 10.6-10.10 the AudioComponent/AudioUnit functions live in AudioUnit.framework; the modern SDK binds them
 * to AudioToolbox, which makes dyld fail at launch on old systems. Resolve them at run time instead. */
static OSStatus (*pAudioComponentInstanceNew)(AudioComponent, AudioComponentInstance *);
static AudioComponent (*pAudioComponentFindNext)(AudioComponent, const AudioComponentDescription *);
static OSStatus (*pAudioUnitSetProperty)(AudioUnit, AudioUnitPropertyID, AudioUnitScope, AudioUnitElement, const void *, UInt32);
static OSStatus (*pAudioUnitInitialize)(AudioUnit);
static OSStatus (*pAudioOutputUnitStart)(AudioUnit);
static int resolve_audio(void) {
    dlopen("/System/Library/Frameworks/AudioUnit.framework/AudioUnit", RTLD_LAZY | RTLD_GLOBAL);
    dlopen("/System/Library/Frameworks/AudioToolbox.framework/AudioToolbox", RTLD_LAZY | RTLD_GLOBAL);
    *(void **)&pAudioComponentFindNext = dlsym(RTLD_DEFAULT, "AudioComponentFindNext");
    *(void **)&pAudioComponentInstanceNew = dlsym(RTLD_DEFAULT, "AudioComponentInstanceNew");
    *(void **)&pAudioUnitSetProperty = dlsym(RTLD_DEFAULT, "AudioUnitSetProperty");
    *(void **)&pAudioUnitInitialize = dlsym(RTLD_DEFAULT, "AudioUnitInitialize");
    *(void **)&pAudioOutputUnitStart = dlsym(RTLD_DEFAULT, "AudioOutputUnitStart");
    return pAudioComponentFindNext && pAudioComponentInstanceNew && pAudioUnitSetProperty && pAudioUnitInitialize && pAudioOutputUnitStart;
}

static AudioUnit gUnit;

static OSStatus render_cb(void *ref, AudioUnitRenderActionFlags *flags, const AudioTimeStamp *ts, UInt32 bus, UInt32 frames, AudioBufferList *io) {
    (void)ref; (void)flags; (void)ts; (void)bus;
    if (io->mNumberBuffers < 1) return noErr;
    audio_render((int16_t *)io->mBuffers[0].mData, (int)frames);
    return noErr;
}

int host_audio_open(void) {
    AudioComponentDescription d = {kAudioUnitType_Output, kAudioUnitSubType_DefaultOutput, kAudioUnitManufacturer_Apple, 0, 0};
    if (!resolve_audio()) { fprintf(stderr, "[audio] AudioUnit functions not found\n"); return 0; }
    AudioComponent c = pAudioComponentFindNext(NULL, &d);
    AudioStreamBasicDescription f;
    AURenderCallbackStruct cb = {render_cb, NULL};
    if (!c || pAudioComponentInstanceNew(c, &gUnit) != noErr) { fprintf(stderr, "[audio] no audio output\n"); return 0; }
    memset(&f, 0, sizeof f);
    f.mSampleRate = 44100; f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    f.mChannelsPerFrame = 2; f.mBitsPerChannel = 16; f.mBytesPerFrame = 4; f.mFramesPerPacket = 1; f.mBytesPerPacket = 4;
    if (pAudioUnitSetProperty(gUnit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f) != noErr ||
        pAudioUnitSetProperty(gUnit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb) != noErr ||
        pAudioUnitInitialize(gUnit) != noErr || pAudioOutputUnitStart(gUnit) != noErr) {
        fprintf(stderr, "[audio] failed to start the AudioUnit\n");
        return 0;
    }
    fprintf(stderr, "[audio] output started (44100 Hz, 16-bit, stereo)\n");
    return 1;
}
