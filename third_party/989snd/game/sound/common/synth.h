// Copyright: 2021 - 2024, Ziemas
// SPDX-License-Identifier: ISC
#pragma once
#include <forward_list>
#include <memory>
#include <unordered_map>
#include <vector>

#include "envelope.h"
#include "sound_types.h"
#include "voice.h"

#include "common/common_types.h"

namespace snd {
struct SpuVolume {
  /*   0 */ s16 left;
  /*   2 */ s16 right;
};

class Synth {
 public:
  Synth() {
    mVolume.left.Set(0x3FFF);
    mVolume.right.Set(0x3FFF);
    mVolume.Snap();
  }

  s16Output Tick();
  void AddVoice(std::shared_ptr<Voice> voice);
  void SetMasterVol(u32 volume);
  void SetReverbMode(s32 mode, s32 delay = 0, s32 feedback = 0);
  void SetReverbDepth(s16 left, s16 right);
  void AutoReverb(s16 depth, s32 delta_time, s32 channel_flags);

 private:
  void StepReverb(s32 revInL, s32 revInR);

  std::forward_list<std::shared_ptr<Voice>> mVoices;

  VolumePair mVolume{};
  VolumePair mReverbDepth{};
  s32 mReverbMode{0};
  s32 mReverbDelay{0};
  s32 mReverbFeedback{0};
  u32 mReverbPos{0};
  u8 mReverbPhase{0};
  s16 mReverbOutL{0};
  s16 mReverbOutR{0};
  std::vector<s16> mReverbBuf{};
};
}  // namespace snd
