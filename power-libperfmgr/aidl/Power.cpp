/*
 * Copyright (C) 2020 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "powerhal-libperfmgr"

#include "Power.h"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/properties.h>
#include <android-base/strings.h>
#include <fmq/AidlMessageQueue.h>
#include <fmq/EventFlag.h>
#include <perfmgr/HintManager.h>
#include <utils/Log.h>

#include <mutex>
#include <optional>

#include "PowerHintSession.h"
#include "PowerSessionManager.h"
#include "disp-power/DisplayLowPower.h"

#define TARGET_TAP_TO_WAKE_NODE "/sys/touchpanel/double_tap"

namespace aidl {
namespace google {
namespace hardware {
namespace power {
namespace impl {
namespace pixel {

using ::aidl::google::hardware::power::impl::pixel::PowerHintSession;
using ::android::perfmgr::HintManager;

using ::aidl::android::hardware::common::fmq::MQDescriptor;
using ::aidl::android::hardware::common::fmq::SynchronizedReadWrite;
using ::aidl::android::hardware::power::ChannelMessage;
using ::android::AidlMessageQueue;

constexpr char kPowerHalStateProp[] = "vendor.powerhal.state";
constexpr char kPowerHalAudioProp[] = "vendor.powerhal.audio";
constexpr char kPowerHalRenderingProp[] = "vendor.powerhal.rendering";

Power::Power(std::shared_ptr<DisplayLowPower> dlpw)
    : mDisplayLowPower(dlpw),
      mInteractionHandler(nullptr),
      mSustainedPerfModeOn(false) {
    mInteractionHandler = std::make_unique<InteractionHandler>();
    mInteractionHandler->Init();

    std::string state = ::android::base::GetProperty(kPowerHalStateProp, "");
    if (state == "SUSTAINED_PERFORMANCE") {
        LOG(INFO) << "Initialize with SUSTAINED_PERFORMANCE on";
        HintManager::GetInstance()->DoHint("SUSTAINED_PERFORMANCE");
        mSustainedPerfModeOn = true;
    } else {
        LOG(INFO) << "Initialize PowerHAL";
    }

    state = ::android::base::GetProperty(kPowerHalAudioProp, "");
    if (state == "AUDIO_STREAMING_LOW_LATENCY") {
        LOG(INFO) << "Initialize with AUDIO_LOW_LATENCY on";
        HintManager::GetInstance()->DoHint(state);
    }

    state = ::android::base::GetProperty(kPowerHalRenderingProp, "");
    if (state == "EXPENSIVE_RENDERING") {
        LOG(INFO) << "Initialize with EXPENSIVE_RENDERING on";
        HintManager::GetInstance()->DoHint("EXPENSIVE_RENDERING");
    }

    auto status = this->getInterfaceVersion(&mServiceVersion);
    LOG(INFO) << "PowerHAL InterfaceVersion:" << mServiceVersion << " isOK: " << status.isOk();
}

ndk::ScopedAStatus Power::setMode(Mode type, bool enabled) {
    LOG(DEBUG) << "Power setMode: " << toString(type) << " to: " << enabled;
    if (HintManager::GetInstance()->GetAdpfProfile() &&
        HintManager::GetInstance()->GetAdpfProfile()->mReportingRateLimitNs > 0) {
        PowerSessionManager::getInstance()->updateHintMode(toString(type), enabled);
    }
    switch (type) {
        case Mode::DOUBLE_TAP_TO_WAKE:
            ::android::base::WriteStringToFile(enabled ? "1" : "0", TARGET_TAP_TO_WAKE_NODE, true);
            break;
        case Mode::LOW_POWER:
            break;
        case Mode::SUSTAINED_PERFORMANCE:
            if (enabled) {
                HintManager::GetInstance()->DoHint("SUSTAINED_PERFORMANCE");
            }
            mSustainedPerfModeOn = true;
            break;
        case Mode::LAUNCH:
            if (mSustainedPerfModeOn) {
                break;
            }
            [[fallthrough]];
        case Mode::FIXED_PERFORMANCE:
            [[fallthrough]];
        case Mode::EXPENSIVE_RENDERING:
            [[fallthrough]];
        case Mode::INTERACTIVE:
            [[fallthrough]];
        case Mode::DEVICE_IDLE:
            [[fallthrough]];
        case Mode::DISPLAY_INACTIVE:
            [[fallthrough]];
        case Mode::AUDIO_STREAMING_LOW_LATENCY:
            [[fallthrough]];
        case Mode::GAME_LOADING:
            [[fallthrough]];
        default:
            if (enabled) {
                HintManager::GetInstance()->DoHint(toString(type));
            } else {
                HintManager::GetInstance()->EndHint(toString(type));
            }
            break;
    }

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Power::isModeSupported(Mode type, bool *_aidl_return) {
    switch (mServiceVersion) {
        case 5:
            if (static_cast<int32_t>(type) <= static_cast<int32_t>(Mode::AUTOMOTIVE_PROJECTION))
                break;
            [[fallthrough]];
        case 4:
            [[fallthrough]];
        case 3:
            if (static_cast<int32_t>(type) <= static_cast<int32_t>(Mode::GAME_LOADING))
                break;
            [[fallthrough]];
        case 2:
            [[fallthrough]];
        case 1:
            if (static_cast<int32_t>(type) <= static_cast<int32_t>(Mode::CAMERA_STREAMING_HIGH))
                break;
            [[fallthrough]];
        default:
            *_aidl_return = false;
            return ndk::ScopedAStatus::ok();
    }
    bool supported = HintManager::GetInstance()->IsHintSupported(toString(type));
    // LOW_POWER and DOUBLE_TAP_TO_WAKE handled insides PowerHAL specifically
    if (type == Mode::LOW_POWER || type == Mode::DOUBLE_TAP_TO_WAKE) {
        supported = true;
    }
    if (!supported && HintManager::GetInstance()->IsAdpfProfileSupported(toString(type))) {
        supported = true;
    }
    LOG(INFO) << "Power mode " << toString(type) << " isModeSupported: " << supported;
    *_aidl_return = supported;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Power::setBoost(Boost type, int32_t durationMs) {
    LOG(DEBUG) << "Power setBoost: " << toString(type) << " duration: " << durationMs;
    if (HintManager::GetInstance()->GetAdpfProfile() &&
        HintManager::GetInstance()->GetAdpfProfile()->mReportingRateLimitNs > 0) {
        PowerSessionManager::getInstance()->updateHintBoost(toString(type), durationMs);
    }
    switch (type) {
        case Boost::INTERACTION:
            if (mSustainedPerfModeOn) {
                break;
            }
            mInteractionHandler->Acquire(durationMs);
            break;
        case Boost::DISPLAY_UPDATE_IMMINENT:
            [[fallthrough]];
        case Boost::ML_ACC:
            [[fallthrough]];
        case Boost::AUDIO_LAUNCH:
            [[fallthrough]];
        default:
            if (mSustainedPerfModeOn) {
                break;
            }
            if (durationMs > 0) {
                HintManager::GetInstance()->DoHint(toString(type),
                                                   std::chrono::milliseconds(durationMs));
            } else if (durationMs == 0) {
                HintManager::GetInstance()->DoHint(toString(type));
            } else {
                HintManager::GetInstance()->EndHint(toString(type));
            }
            break;
    }

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Power::isBoostSupported(Boost type, bool *_aidl_return) {
    switch (mServiceVersion) {
        case 5:
            [[fallthrough]];
        case 4:
            [[fallthrough]];
        case 3:
            [[fallthrough]];
        case 2:
            [[fallthrough]];
        case 1:
            if (static_cast<int32_t>(type) <= static_cast<int32_t>(Boost::CAMERA_SHOT))
                break;
            [[fallthrough]];
        default:
            *_aidl_return = false;
            return ndk::ScopedAStatus::ok();
    }
    bool supported = HintManager::GetInstance()->IsHintSupported(toString(type));
    if (!supported && HintManager::GetInstance()->IsAdpfProfileSupported(toString(type))) {
        supported = true;
    }
    LOG(INFO) << "Power boost " << toString(type) << " isBoostSupported: " << supported;
    *_aidl_return = supported;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Power::createHintSession(int32_t tgid, int32_t uid,
                                            const std::vector<int32_t> &threadIds,
                                            int64_t durationNanos,
                                            std::shared_ptr<IPowerHintSession> *_aidl_return) {
    SessionConfig config;
    return createHintSessionWithConfig(tgid, uid, threadIds, durationNanos, SessionTag::OTHER,
                                       &config, _aidl_return);
}

ndk::ScopedAStatus Power::getHintSessionPreferredRate(int64_t *outNanoseconds) {
    *outNanoseconds = HintManager::GetInstance()->GetAdpfProfile()
                              ? HintManager::GetInstance()->GetAdpfProfile()->mReportingRateLimitNs
                              : 0;
    if (*outNanoseconds <= 0) {
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }

    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Power::createHintSessionWithConfig(
        int32_t tgid, int32_t uid, const std::vector<int32_t> &threadIds, int64_t durationNanos,
        SessionTag tag, SessionConfig *config, std::shared_ptr<IPowerHintSession> *_aidl_return) {
    if (!HintManager::GetInstance()->GetAdpfProfile() ||
        HintManager::GetInstance()->GetAdpfProfile()->mReportingRateLimitNs <= 0) {
        *_aidl_return = nullptr;
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }
    if (threadIds.size() == 0) {
        LOG(ERROR) << "Error: threadIds.size() shouldn't be " << threadIds.size();
        *_aidl_return = nullptr;
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    std::shared_ptr<IPowerHintSession> session =
            ndk::SharedRefBase::make<PowerHintSession>(tgid, uid, threadIds, durationNanos, tag);
    *_aidl_return = session;
    static_cast<PowerHintSession *>(_aidl_return->get())->getSessionConfig(config);
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Power::getSessionChannel(int32_t, int32_t, ChannelConfig *_aidl_return) {
    static AidlMessageQueue<ChannelMessage, SynchronizedReadWrite> stubQueue{20, true};
    static std::thread stubThread([&] {
        ChannelMessage data;
        // This loop will only run while there is data waiting
        // to be processed, and blocks on a futex all other times
        while (stubQueue.readBlocking(&data, 1, 0)) {
        }
    });
    _aidl_return->channelDescriptor = stubQueue.dupeDesc();
    _aidl_return->readFlagBitmask = 0x01;
    _aidl_return->writeFlagBitmask = 0x02;
    _aidl_return->eventFlagDescriptor = std::nullopt;
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Power::closeSessionChannel(int32_t, int32_t) {
    return ndk::ScopedAStatus::ok();
}

}  // namespace pixel
}  // namespace impl
}  // namespace power
}  // namespace hardware
}  // namespace google
}  // namespace aidl
