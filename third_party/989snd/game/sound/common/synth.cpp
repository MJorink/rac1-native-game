// Copyright: 2021 - 2024, Ziemas
// SPDX-License-Identifier: ISC
#include "synth.h"

#include <algorithm>
#include <array>
#include <stdexcept>

namespace snd {

namespace {
struct ReverbPreset {
  u16 dAPF1, dAPF2;
  s16 vIIR, vCOMB1, vCOMB2, vCOMB3, vCOMB4, vWALL, vAPF1, vAPF2;
  u16 mLSAME, mRSAME, mLCOMB1, mRCOMB1, mLCOMB2, mRCOMB2, dLSAME, dRSAME;
  u16 mLDIFF, mRDIFF, mLCOMB3, mRCOMB3, mLCOMB4, mRCOMB4, dLDIFF, dRDIFF;
  u16 mLAPF1, mRAPF1, mLAPF2, mRAPF2;
  s16 vLIN, vRIN;
};

constexpr std::array<ReverbPreset, 10> kReverbPresets = {{
    // 0: OFF
    {},
    // 1: ROOM
    {0x007D, 0x005B, 0x6D80, 0x54B8, -0x4130, 0x0000, 0x0000, -0x4580, 0x5800, 0x5300, 0x04D6,
     0x0333, 0x03F0, 0x0227, 0x0374, 0x01EF, 0x0334, 0x01B5, 0x0000, 0x0000, 0x0000, 0x0000,
     0x0000, 0x0000, 0x0000, 0x0000, 0x01B4, 0x0136, 0x00B8, 0x005C, -0x8000, -0x8000},
    // 2: STUDIO_A
    {0x0033, 0x0025, 0x70F0, 0x4FA8, -0x4320, 0x4410, -0x3F10, -0x6390, 0x5280, 0x4EC0, 0x03E4,
     0x031B, 0x03A4, 0x02AF, 0x0372, 0x0266, 0x031C, 0x025D, 0x025C, 0x018E, 0x022F, 0x0135,
     0x01D2, 0x00B7, 0x018F, 0x00B5, 0x00B4, 0x0080, 0x004C, 0x0026, -0x8000, -0x8000},
    // 3: STUDIO_B
    {0x00B1, 0x007F, 0x70F0, 0x4FA8, -0x4320, 0x4510, -0x4110, -0x4B40, 0x5280, 0x4EC0, 0x0904,
     0x076B, 0x0824, 0x065F, 0x07A2, 0x0616, 0x076C, 0x05ED, 0x05EC, 0x042E, 0x050F, 0x0305,
     0x0462, 0x02B7, 0x042F, 0x0265, 0x0264, 0x01B2, 0x0100, 0x0080, -0x8000, -0x8000},
    // 4: STUDIO_C
    {0x00E3, 0x00A9, 0x6F60, 0x4FA8, -0x4320, 0x4510, -0x4110, -0x4980, 0x5680, 0x52C0, 0x0DFB,
     0x0B58, 0x0D09, 0x0A3C, 0x0BD9, 0x0973, 0x0B59, 0x08DA, 0x08D9, 0x05E9, 0x07EC, 0x04B0,
     0x06EF, 0x03D2, 0x05EA, 0x031D, 0x031C, 0x0238, 0x0154, 0x00AA, -0x8000, -0x8000},
    // 5: HALL
    {0x01A5, 0x0139, 0x6000, 0x5000, 0x4C00, -0x4800, -0x4400, -0x4000, 0x6000, 0x5C00, 0x15BA,
     0x11BB, 0x14C2, 0x10BD, 0x11BC, 0x0DC1, 0x11C0, 0x0DC3, 0x0DC0, 0x09C1, 0x0BC4, 0x07C1,
     0x0A00, 0x06CD, 0x09C2, 0x05C1, 0x05C0, 0x041A, 0x0274, 0x013A, -0x8000, -0x8000},
    // 6: SPACE
    {0x033D, 0x0231, 0x7E00, 0x5000, -0x4C00, -0x5000, 0x4C00, -0x5000, 0x6000, 0x5400, 0x1ED6,
     0x1A31, 0x1D14, 0x183B, 0x1BC2, 0x16B2, 0x1A32, 0x15EF, 0x15EE, 0x1055, 0x1334, 0x0F2D,
     0x11F6, 0x0C5D, 0x1056, 0x0AE1, 0x0AE0, 0x07A2, 0x0464, 0x0232, -0x8000, -0x8000},
    // 7: ECHO
    {0x0001, 0x0001, 0x7FFF, 0x7FFF, 0x0000, 0x0000, 0x0000, -0x7F00, 0x0000, 0x0000, 0x1FFF,
     0x0FFF, 0x1005, 0x0005, 0x0000, 0x0000, 0x1005, 0x0005, 0x0000, 0x0000, 0x0000, 0x0000,
     0x0000, 0x0000, 0x0000, 0x0000, 0x1004, 0x1002, 0x0004, 0x0002, -0x8000, -0x8000},
    // 8: DELAY
    {0x0001, 0x0001, 0x7FFF, 0x7FFF, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x0000, 0x1FFF,
     0x0FFF, 0x1005, 0x0005, 0x0000, 0x0000, 0x1005, 0x0005, 0x0000, 0x0000, 0x0000, 0x0000,
     0x0000, 0x0000, 0x0000, 0x0000, 0x1004, 0x1002, 0x0004, 0x0002, -0x8000, -0x8000},
    // 9: PIPE
    {0x0017, 0x0013, 0x6D80, 0x54B8, -0x4130, 0x0000, 0x0000, -0x3F00, 0x5800, 0x5300, 0x00DF,
     0x00B8, 0x00C9, 0x00A1, 0x00BD, 0x0099, 0x00B9, 0x008A, 0x0089, 0x0060, 0x0075, 0x0052,
     0x006B, 0x0046, 0x0061, 0x003E, 0x003D, 0x002B, 0x001E, 0x0014, -0x8000, -0x8000},
}};

s16 Clamp16(s64 v) {
  return static_cast<s16>(std::clamp<s64>(v, INT16_MIN, INT16_MAX));
}
}  // namespace

static s16 ApplyVolume(s16 sample, s32 volume) {
  return static_cast<s16>((sample * volume) >> 15);
}

void Synth::StepReverb(s32 revInL, s32 revInR) {
  if (mReverbMode <= 0 || mReverbMode >= static_cast<s32>(kReverbPresets.size())) {
    mReverbOutL = 0;
    mReverbOutR = 0;
    return;
  }
  if (mReverbBuf.size() < 0x10000) {
    mReverbBuf.assign(0x10000, 0);
  }

  mReverbPhase ^= 1;
  if (mReverbPhase == 0) {
    return;
  }

  ReverbPreset r = kReverbPresets[mReverbMode];
  if (mReverbMode == 7 || mReverbMode == 8) {
    u16 d = static_cast<u16>(std::clamp<s32>(mReverbDelay, 0, 127) << 4);
    r.mLSAME = static_cast<u16>(0x1004 + d);
    r.mRSAME = static_cast<u16>(0x0004 + d);
    r.vWALL = static_cast<s16>(std::clamp<s32>((mReverbFeedback * 0x8000) / 128, -0x8000, 0x7FFF));
  }

  auto rd = [&](u32 off) -> s64 { return mReverbBuf[(mReverbPos + (off << 2)) & 0xFFFF]; };
  auto rd1 = [&](u32 off) -> s64 { return mReverbBuf[(mReverbPos + (off << 2) - 1) & 0xFFFF]; };
  auto wr = [&](u32 off, s16 val) { mReverbBuf[(mReverbPos + (off << 2)) & 0xFFFF] = val; };

  s32 Lin = (Clamp16(revInL) * static_cast<s32>(r.vLIN)) >> 15;
  s32 Rin = (Clamp16(revInR) * static_cast<s32>(r.vRIN)) >> 15;

  wr(r.mLSAME,
     Clamp16((((Lin + ((rd(r.dLSAME) * r.vWALL) >> 15) - rd1(r.mLSAME)) * r.vIIR) >> 15) +
             rd1(r.mLSAME)));
  wr(r.mRSAME,
     Clamp16((((Rin + ((rd(r.dRSAME) * r.vWALL) >> 15) - rd1(r.mRSAME)) * r.vIIR) >> 15) +
             rd1(r.mRSAME)));

  wr(r.mLDIFF,
     Clamp16((((Lin + ((rd(r.dRDIFF) * r.vWALL) >> 15) - rd1(r.mLDIFF)) * r.vIIR) >> 15) +
             rd1(r.mLDIFF)));
  wr(r.mRDIFF,
     Clamp16((((Rin + ((rd(r.dLDIFF) * r.vWALL) >> 15) - rd1(r.mRDIFF)) * r.vIIR) >> 15) +
             rd1(r.mRDIFF)));

  s32 Lout = Clamp16((r.vCOMB1 * rd(r.mLCOMB1) + r.vCOMB2 * rd(r.mLCOMB2) +
                      r.vCOMB3 * rd(r.mLCOMB3) + r.vCOMB4 * rd(r.mLCOMB4)) >>
                     15);
  s32 Rout = Clamp16((r.vCOMB1 * rd(r.mRCOMB1) + r.vCOMB2 * rd(r.mRCOMB2) +
                      r.vCOMB3 * rd(r.mRCOMB3) + r.vCOMB4 * rd(r.mRCOMB4)) >>
                     15);

  s64 lapf1 = rd(r.mLAPF1 - r.dAPF1);
  Lout = Clamp16(Lout - ((r.vAPF1 * lapf1) >> 15));
  wr(r.mLAPF1, static_cast<s16>(Lout));
  Lout = Clamp16(((Lout * r.vAPF1) >> 15) + lapf1);

  s64 rapf1 = rd(r.mRAPF1 - r.dAPF1);
  Rout = Clamp16(Rout - ((r.vAPF1 * rapf1) >> 15));
  wr(r.mRAPF1, static_cast<s16>(Rout));
  Rout = Clamp16(((Rout * r.vAPF1) >> 15) + rapf1);

  s64 lapf2 = rd(r.mLAPF2 - r.dAPF2);
  Lout = Clamp16(Lout - ((r.vAPF2 * lapf2) >> 15));
  wr(r.mLAPF2, static_cast<s16>(Lout));
  Lout = Clamp16(((Lout * r.vAPF2) >> 15) + lapf2);

  s64 rapf2 = rd(r.mRAPF2 - r.dAPF2);
  Rout = Clamp16(Rout - ((r.vAPF2 * rapf2) >> 15));
  wr(r.mRAPF2, static_cast<s16>(Rout));
  Rout = Clamp16(((Rout * r.vAPF2) >> 15) + rapf2);

  mReverbOutL = static_cast<s16>(Lout);
  mReverbOutR = static_cast<s16>(Rout);
  mReverbPos = (mReverbPos + 1) & 0xFFFF;
}

s16Output Synth::Tick() {
  s32 dryL = 0;
  s32 dryR = 0;
  s32 revInL = 0;
  s32 revInR = 0;

  mVoices.remove_if([](std::shared_ptr<Voice>& v) { return v->Dead(); });
  for (auto& v : mVoices) {
    s16Output vo = v->Run();
    if (!v->IsReverbOnly()) {
      dryL += vo.left;
      dryR += vo.right;
    }
    if (v->IsReverb()) {
      revInL += vo.left;
      revInR += vo.right;
    }
  }

  if (mReverbMode > 0) {
    StepReverb(revInL, revInR);
    dryL += ApplyVolume(mReverbOutL, mReverbDepth.left.GetCurrent());
    dryR += ApplyVolume(mReverbOutR, mReverbDepth.right.GetCurrent());
    mReverbDepth.Run();
  }

  s16Output out{Clamp16(dryL), Clamp16(dryR)};
  out.left = ApplyVolume(out.left, mVolume.left.GetCurrent());
  out.right = ApplyVolume(out.right, mVolume.right.GetCurrent());

  mVolume.Run();

  return out;
}

void Synth::AddVoice(std::shared_ptr<Voice> voice) {
  mVoices.emplace_front(voice);
}

void Synth::SetMasterVol(u32 volume) {
  mVolume.left.Set(volume);
  mVolume.right.Set(volume);
}

void Synth::SetReverbMode(s32 mode, s32 delay, s32 feedback) {
  s32 clamped = std::clamp<s32>(mode, 0, 9);
  if (clamped != mReverbMode) {
    mReverbMode = clamped;
    if (mReverbBuf.size() < 0x10000) {
      mReverbBuf.assign(0x10000, 0);
    } else {
      std::fill(mReverbBuf.begin(), mReverbBuf.end(), 0);
    }
    mReverbOutL = 0;
    mReverbOutR = 0;
  }
  mReverbDelay = delay;
  mReverbFeedback = feedback;
}

void Synth::SetReverbDepth(s16 left, s16 right) {
  u16 l = static_cast<u16>((left >> 1) & 0x7FFF);  // level = the depth (its low bit dropped)
  u16 r = static_cast<u16>((right >> 1) & 0x7FFF);
  mReverbDepth.left.Set(l);
  mReverbDepth.right.Set(r);
}

void Synth::AutoReverb(s16 depth, s32 /*delta_time*/, s32 channel_flags) {
  u16 d = static_cast<u16>((depth >> 1) & 0x7FFF);
  if (channel_flags & 1) {
    mReverbDepth.left.Set(d);
  }
  if (channel_flags & 2) {
    mReverbDepth.right.Set(d);
  }
}
}  // namespace snd
