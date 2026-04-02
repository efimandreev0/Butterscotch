#include "noop_audio_system.h"

#include <stdlib.h>

static void noopInit([[maybe_unused]] AudioSystem* audio, [[maybe_unused]] DataWin* dataWin, [[maybe_unused]] FileSystem* fileSystem) {}

static void noopDestroy(AudioSystem* audio) {
    free(audio);
}

static void noopUpdate([[maybe_unused]] AudioSystem* audio, [[maybe_unused]] float deltaTime) {}

static int32_t noopPlaySound([[maybe_unused]] AudioSystem* audio, [[maybe_unused]] int32_t soundIndex, [[maybe_unused]] int32_t priority, [[maybe_unused]] bool loop) {
    return -1;
}

static void noopStopSound([[maybe_unused]] AudioSystem* audio, [[maybe_unused]] int32_t soundOrInstance) {}

static void noopStopAll([[maybe_unused]] AudioSystem* audio) {}

static bool noopIsPlaying([[maybe_unused]] AudioSystem* audio, [[maybe_unused]] int32_t soundOrInstance) {
    return false;
}

static void noopPauseSound([[maybe_unused]] AudioSystem* audio, [[maybe_unused]] int32_t soundOrInstance) {}

static void noopResumeSound([[maybe_unused]] AudioSystem* audio, [[maybe_unused]] int32_t soundOrInstance) {}

static void noopPauseAll([[maybe_unused]] AudioSystem* audio) {}

static void noopResumeAll([[maybe_unused]] AudioSystem* audio) {}

static void noopSetSoundGain([[maybe_unused]] AudioSystem* audio, [[maybe_unused]] int32_t soundOrInstance, [[maybe_unused]] float gain, [[maybe_unused]] uint32_t timeMs) {}

static float noopGetSoundGain([[maybe_unused]] AudioSystem* audio, [[maybe_unused]] int32_t soundOrInstance) {
    return 1.0f;
}

static void noopSetSoundPitch([[maybe_unused]] AudioSystem* audio, [[maybe_unused]] int32_t soundOrInstance, [[maybe_unused]] float pitch) {}

static float noopGetSoundPitch([[maybe_unused]] AudioSystem* audio, [[maybe_unused]] int32_t soundOrInstance) {
    return 1.0f;
}

static float noopGetTrackPosition([[maybe_unused]] AudioSystem* audio, [[maybe_unused]] int32_t soundOrInstance) {
    return 0.0f;
}

static void noopSetTrackPosition([[maybe_unused]] AudioSystem* audio, [[maybe_unused]] int32_t soundOrInstance, [[maybe_unused]] float positionSeconds) {}

static void noopSetMasterGain([[maybe_unused]] AudioSystem* audio, [[maybe_unused]] float gain) {}

static void noopSetChannelCount([[maybe_unused]] AudioSystem* audio, [[maybe_unused]] int32_t count) {}

static void noopGroupLoad([[maybe_unused]] AudioSystem* audio, [[maybe_unused]] int32_t groupIndex) {}

static bool noopGroupIsLoaded([[maybe_unused]] AudioSystem* audio, [[maybe_unused]] int32_t groupIndex) {
    return true;
}

static int32_t noopCreateStream([[maybe_unused]] AudioSystem* audio, [[maybe_unused]] const char* filename) {
    return -1;
}

static bool noopDestroyStream([[maybe_unused]] AudioSystem* audio, [[maybe_unused]] int32_t streamIndex) {
    return false;
}

static AudioSystemVtable noopVtable = {
    .init = noopInit,
    .destroy = noopDestroy,
    .update = noopUpdate,
    .playSound = noopPlaySound,
    .stopSound = noopStopSound,
    .stopAll = noopStopAll,
    .isPlaying = noopIsPlaying,
    .pauseSound = noopPauseSound,
    .resumeSound = noopResumeSound,
    .pauseAll = noopPauseAll,
    .resumeAll = noopResumeAll,
    .setSoundGain = noopSetSoundGain,
    .getSoundGain = noopGetSoundGain,
    .setSoundPitch = noopSetSoundPitch,
    .getSoundPitch = noopGetSoundPitch,
    .getTrackPosition = noopGetTrackPosition,
    .setTrackPosition = noopSetTrackPosition,
    .setMasterGain = noopSetMasterGain,
    .setChannelCount = noopSetChannelCount,
    .groupLoad = noopGroupLoad,
    .groupIsLoaded = noopGroupIsLoaded,
    .createStream = noopCreateStream,
    .destroyStream = noopDestroyStream,
};

NoopAudioSystem* NoopAudioSystem_create(void) {
    NoopAudioSystem* audio = calloc(1, sizeof(NoopAudioSystem));
    audio->base.vtable = &noopVtable;
    return audio;
}
