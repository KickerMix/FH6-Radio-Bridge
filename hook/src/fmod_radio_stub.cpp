#include "fmod_radio_stub.h"

namespace fh6rb
{
bool FmodRadioPcmProvider::TryConnect()
{
    return reader_.TryConnect();
}

bool FmodRadioPcmProvider::FillFloat32Stereo(float* outputInterleavedStereo, uint32_t frameCount)
{
    return reader_.ReadFrames(outputInterleavedStereo, frameCount);
}

SharedAudioStatus FmodRadioPcmProvider::GetStatus() const
{
    return reader_.GetStatus();
}
}
