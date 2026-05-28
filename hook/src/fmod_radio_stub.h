#pragma once

#include "shared_audio_reader.h"

#include <cstdint>

namespace fh6rb
{
class FmodRadioPcmProvider
{
public:
    bool TryConnect();

    // Intended shape for a future FMOD PCM read callback.
    // The caller owns outputInterleavedStereo and passes frame count, not bytes.
    // Returns true only when the full requested frame block was copied.
    bool FillFloat32Stereo(float* outputInterleavedStereo, uint32_t frameCount);

    SharedAudioStatus GetStatus() const;

private:
    SharedAudioRingReader reader_;
};
}
