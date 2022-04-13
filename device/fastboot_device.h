/*
 * Copyright (C) 2018 The Android Open Source Project
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

#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <android/hardware/boot/1.0/IBootControl.h>
#include <android/hardware/boot/1.1/IBootControl.h>
#include <android/hardware/fastboot/1.1/IFastboot.h>
#include <android/hardware/health/2.0/IHealth.h>

#include "commands.h"
#include "variables.h"

class FastbootDevice {
  public:
    FastbootDevice();
    ~FastbootDevice();

    void CloseDevice();
    void ExecuteCommand(char* command);
    bool WriteStatus(FastbootResult result, const std::string& message);
    std::string GetCurrentSlot();

    // Shortcuts for writing status results.
    bool WriteOkay(const std::string& message);
    bool WriteFail(const std::string& message);
    bool WriteInfo(const std::string& message);

    ssize_t StreamDownload(const void* buf, size_t size);

    FastbootResult get_result() { return result_; }
    std::string get_message() { return message_; }
    std::vector<std::string> get_info() { return info_; }

    std::vector<char>& download_data() { return download_data_; }
    android::sp<android::hardware::boot::V1_0::IBootControl> boot_control_hal() {
        return boot_control_hal_;
    }
    android::sp<android::hardware::boot::V1_1::IBootControl> boot1_1() { return boot1_1_; }
    android::sp<android::hardware::fastboot::V1_1::IFastboot> fastboot_hal() {
        return fastboot_hal_;
    }
    android::sp<android::hardware::health::V2_0::IHealth> health_hal() { return health_hal_; }

    void set_active_slot(const std::string& active_slot) { active_slot_ = active_slot; }

  private:
    const std::unordered_map<std::string, CommandHandler> kCommandMap;

    FastbootResult result_;
    std::string message_;
    std::vector<std::string> info_;

    android::sp<android::hardware::boot::V1_0::IBootControl> boot_control_hal_;
    android::sp<android::hardware::boot::V1_1::IBootControl> boot1_1_;
    android::sp<android::hardware::health::V2_0::IHealth> health_hal_;
    android::sp<android::hardware::fastboot::V1_1::IFastboot> fastboot_hal_;
    std::vector<char> download_data_;
    uint64_t download_offset_;
    std::string active_slot_;
};
