/*
 * SPDX-FileCopyrightText: 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */

#define LOG_TAG "libqtivibratoreffect.xiaomi"

#include "AacEffect.h"

#include <aidl/android/hardware/vibrator/CompositePrimitive.h>
#include <log/log.h>

#include <algorithm>
#include <array>
#include <cmath>

#ifndef XIAOMI_VIBRATOR_AAC_F0
#error "Set xiaomi_vibrator.aac_f0 when enabling the AAC effect generator"
#endif

// The AAC MI Haptics API in the stock libaachaptics.so has process-global state.
extern "C" {
int aac_haptics_init(int32_t* config);
int aac_haptics_process(void* events, uint32_t size);
int aac_haptics_get_frame(void* buffer, int32_t* length);
}

namespace {

using aidl::android::hardware::vibrator::CompositePrimitive;
using HeEvent = std::array<int32_t, 17>;
static_assert(sizeof(HeEvent) == 68);

constexpr int32_t kSampleRateHz = 24000;
constexpr int32_t kFrameSize = 512;
constexpr int32_t kMaxPrimitiveDurationMs = 500;
// AAC includes the final sample at the envelope endpoint.
constexpr size_t kMaxSamples = kSampleRateHz * kMaxPrimitiveDurationMs / 1000 + 1;
// Each of the three envelope segments can end with a partial frame. Allow
// those boundaries and the zero-length terminal frame as well as full frames.
constexpr size_t kMaxFrameCount = (kMaxSamples + kFrameSize - 1) / kFrameSize + 3;
constexpr int32_t kF0Hz = XIAOMI_VIBRATOR_AAC_F0;
static_assert(kF0Hz >= 150 && kF0Hz <= 190);

// Transient sharpness selects an engine waveform; it is not a frequency in Hz.
// The engine determines the duration from both sharpness and intensity.
constexpr int32_t kTransientClickSharpness = 50;
constexpr int32_t kTransientLightTickSharpness = 80;
constexpr int32_t kTransientLowTickSharpness = 10;
constexpr float kClickAmplitude = 1.0f;
constexpr float kLightTickAmplitude = 0.5f;
constexpr float kLowTickAmplitude = 0.8f;

constexpr int32_t kThudDurationMs = 300;
constexpr int32_t kSpinDurationMs = 150;
constexpr int32_t kQuickRiseDurationMs = 150;
constexpr int32_t kSlowRiseDurationMs = 500;
constexpr int32_t kQuickFallDurationMs = 100;
// Keep the peak inside the engine's mandatory zero-amplitude endpoints.
constexpr int32_t kEnvelopeEdgeMs = 5;

// AAC maps the event sharpness plus each point's offset through this
// frequency table. Targets below 107 Hz use the lowest supported frequency.
constexpr std::array<int32_t, 151> kContinuousFrequenciesHz = {
        107, 109, 109, 111, 111, 113, 113, 115, 115, 116, 116, 118, 118, 120, 120,
        122, 122, 124, 124, 126, 126, 128, 128, 130, 130, 131, 131, 133, 133, 135,
        135, 137, 137, 139, 139, 141, 141, 143, 143, 145, 145, 146, 146, 148, 148,
        150, 150, 152, 152, 154, 154, 159, 159, 164, 164, 169, 169, 174, 174, 179,
        179, 184, 184, 189, 189, 194, 194, 199, 199, 204, 204, 209, 209, 214, 214,
        220, 220, 225, 225, 230, 230, 235, 235, 240, 240, 245, 245, 250, 250, 255,
        255, 260, 260, 265, 265, 270, 270, 275, 275, 280, 280, 287, 287, 294, 294,
        302, 302, 309, 309, 316, 316, 323, 323, 330, 330, 338, 338, 345, 345, 352,
        352, 359, 359, 366, 366, 374, 374, 381, 381, 388, 388, 395, 395, 400, 400,
        400, 400, 400, 400, 400, 400, 400, 400, 400, 400, 400, 400, 400, 400, 400,
        400};

struct Point {
    int32_t timeMs;
    float amplitude;
    int32_t frequencyHz;
};

int32_t frequencySelector(int32_t frequencyHz) {
    const auto begin = kContinuousFrequenciesHz.begin();
    const auto end = kContinuousFrequenciesHz.end();
    auto it = std::lower_bound(begin, end, frequencyHz);
    if (it == end) return kContinuousFrequenciesHz.size() - 1;
    if (it != begin && frequencyHz - *(it - 1) <= *it - frequencyHz) --it;
    return it - begin;
}

// HE intensity is nonlinear: drive = I * 1.14^((I - 100) / 10) / 100.
int32_t heIntensity(float amplitude) {
    amplitude = std::clamp(amplitude, 0.0f, 1.0f);
    if (amplitude == 0.0f) return 0;
    float low = 0.0f;
    float high = 100.0f;
    for (int i = 0; i < 20; ++i) {
        const float mid = (low + high) / 2;
        const float drive = mid * std::pow(1.14f, (mid - 100.0f) / 10.0f) / 100.0f;
        (drive < amplitude ? low : high) = mid;
    }
    return std::max(1, static_cast<int32_t>(std::lround(high)));
}

HeEvent transient(float amplitude, int32_t sharpness) {
    return {0x1001, 0, heIntensity(amplitude), sharpness};
}

HeEvent continuous(int32_t durationMs, const std::array<Point, 4>& curve) {
    if (curve.front().amplitude != 0.0f || curve.back().amplitude != 0.0f) {
        ALOGE("AAC continuous envelopes require zero endpoint amplitudes");
        return {};
    }

    std::array<int32_t, 4> selectors;
    for (size_t i = 0; i < curve.size(); ++i) {
        selectors[i] = frequencySelector(curve[i].frequencyHz);
    }
    const auto [low, high] = std::minmax_element(selectors.begin(), selectors.end());
    const int32_t sharpness = std::clamp((*low + *high) / 2, 0, 100);
    if (*low < sharpness - 50 || *high > sharpness + 50) {
        ALOGE("AAC frequency sweep exceeds the control point offset range");
        return {};
    }

    HeEvent event{0x1000, 0, 100, sharpness, durationMs};
    for (size_t i = 0; i < curve.size(); ++i) {
        event[5 + i * 3] = curve[i].timeMs;
        // The engine applies the transfer function after multiplying event and point
        // intensity. Invert each control point to preserve the intended envelope.
        event[6 + i * 3] = heIntensity(curve[i].amplitude);
        event[7 + i * 3] = selectors[i] - sharpness;
    }
    return event;
}

HeEvent primitiveEvent(CompositePrimitive primitive) {
    switch (primitive) {
        case CompositePrimitive::CLICK:
            return transient(kClickAmplitude, kTransientClickSharpness);
        case CompositePrimitive::LIGHT_TICK:
            return transient(kLightTickAmplitude, kTransientLightTickSharpness);
        case CompositePrimitive::LOW_TICK:
            return transient(kLowTickAmplitude, kTransientLowTickSharpness);
        case CompositePrimitive::THUD:
            return continuous(
                    kThudDurationMs,
                    {{{0, 0.0f, kF0Hz},
                      {kEnvelopeEdgeMs, 1.0f, kF0Hz * 7 / 8},
                      {kThudDurationMs / 2, 0.5f, kF0Hz * 5 / 8},
                      {kThudDurationMs, 0.0f, kF0Hz / 2}}});
        case CompositePrimitive::SPIN:
            return continuous(
                    kSpinDurationMs,
                    {{{0, 0.0f, kF0Hz * 2 / 3},
                      {kSpinDurationMs / 2, 1.0f, kF0Hz},
                      {kSpinDurationMs * 3 / 4, 0.6f, kF0Hz * 3 / 4},
                      {kSpinDurationMs, 0.0f, kF0Hz / 2}}});
        case CompositePrimitive::QUICK_RISE:
            return continuous(
                    kQuickRiseDurationMs,
                    {{{0, 0.0f, kF0Hz / 2},
                      {kQuickRiseDurationMs * 2 / 5, 0.45f, kF0Hz * 3 / 4},
                      {kQuickRiseDurationMs - kEnvelopeEdgeMs, 1.0f, kF0Hz},
                      {kQuickRiseDurationMs, 0.0f, kF0Hz}}});
        case CompositePrimitive::SLOW_RISE:
            return continuous(
                    kSlowRiseDurationMs,
                    {{{0, 0.0f, kF0Hz / 2},
                      {kSlowRiseDurationMs * 2 / 5, 0.3f, kF0Hz * 3 / 4},
                      {kSlowRiseDurationMs - kEnvelopeEdgeMs, 1.0f, kF0Hz},
                      {kSlowRiseDurationMs, 0.0f, kF0Hz}}});
        case CompositePrimitive::QUICK_FALL:
            return continuous(
                    kQuickFallDurationMs,
                    {{{0, 0.0f, kF0Hz * 2},
                      {kEnvelopeEdgeMs, 1.0f, kF0Hz * 7 / 4},
                      {kQuickFallDurationMs / 2, 0.3f, kF0Hz * 5 / 4},
                      {kQuickFallDurationMs, 0.0f, kF0Hz}}});
        default:
            return {};
    }
}

class AacEngine {
  public:
    AacEngine() {
        int32_t config[]{kFrameSize, kSampleRateHz, 1, kF0Hz};
        mReady = aac_haptics_init(config) == 0;
        if (!mReady) ALOGE("AAC initialization failed");
    }

    std::vector<int8_t> render(HeEvent event) {
        if (!mReady) return {};
        if (aac_haptics_process(event.data(), sizeof(event)) != 0) {
            mReady = false;
            ALOGE("AAC processing failed");
            return {};
        }

        std::vector<int8_t> samples;
        std::array<int8_t, kFrameSize> frame;
        for (size_t count = 0; count < kMaxFrameCount; ++count) {
            int32_t length = -1;
            if (aac_haptics_get_frame(frame.data(), &length) != 0 || length < 0 ||
                length > kFrameSize || samples.size() + length > kMaxSamples) {
                break;
            }
            if (length == 0) return samples;
            samples.insert(samples.end(), frame.begin(), frame.begin() + length);
        }

        // Do not submit another event after a malformed or incomplete stream.
        mReady = false;
        ALOGE("AAC returned an invalid or oversized stream");
        return {};
    }

  private:
    bool mReady = false;
};

}  // namespace

std::vector<int8_t> renderAacPrimitive(uint32_t primitiveId) {
    const auto primitive = static_cast<CompositePrimitive>(primitiveId);
    if (primitive == CompositePrimitive::NOOP) return std::vector<int8_t>(24, 0);

    auto event = primitiveEvent(primitive);
    if (event[0] == 0) return {};

    // Initialize once, consume each stream through its zero-length terminal frame,
    // and retain the library for the lifetime of the HAL's detached AAC worker.
    static AacEngine engine;
    auto samples = engine.render(event);
    if (!samples.empty()) {
        ALOGI("AAC synthesized %s: %zu samples, %.2f ms at %d Hz (f0=%d Hz)",
              toString(primitive).c_str(), samples.size(),
              samples.size() * 1000.0 / kSampleRateHz, kSampleRateHz, kF0Hz);
    }
    return samples;
}
