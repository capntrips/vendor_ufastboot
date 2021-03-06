/*
 * Copyright (C) 2018 The Android Open Source Project
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *  * Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include "fastboot_driver.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <memory>
#include <regex>
#include <vector>

#include <android-base/file.h>
#include <android-base/mapped_file.h>
#include <android-base/parseint.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>
#include <android-base/unique_fd.h>
#include <storage_literals/storage_literals.h>

#include "constants.h"

using android::base::StringPrintf;
using namespace android::storage_literals;

namespace fastboot {

/*************************** PUBLIC *******************************/
FastBootDriver::FastBootDriver(DriverCallbacks driver_callbacks,
                               bool no_checks)
    : prolog_(std::move(driver_callbacks.prolog)),
      epilog_(std::move(driver_callbacks.epilog)),
      info_(std::move(driver_callbacks.info)),
      disable_checks_(no_checks) {}

FastBootDriver::~FastBootDriver() {
}

RetCode FastBootDriver::Boot(std::string* response, std::vector<std::string>* info) {
    return RawCommand(FB_CMD_BOOT, "Booting", response, info);
}

RetCode FastBootDriver::Continue(std::string* response, std::vector<std::string>* info) {
    return RawCommand(FB_CMD_CONTINUE, "Resuming boot", response, info);
}

RetCode FastBootDriver::CreatePartition(const std::string& partition, const std::string& size) {
    return RawCommand(FB_CMD_CREATE_PARTITION ":" + partition + ":" + size,
                      "Creating '" + partition + "'");
}

RetCode FastBootDriver::DeletePartition(const std::string& partition) {
    return RawCommand(FB_CMD_DELETE_PARTITION ":" + partition, "Deleting '" + partition + "'");
}

RetCode FastBootDriver::Erase(const std::string& partition, std::string* response,
                              std::vector<std::string>* info) {
    return RawCommand(FB_CMD_ERASE ":" + partition, "Erasing '" + partition + "'", response, info);
}

RetCode FastBootDriver::Flash(const std::string& partition, std::string* response,
                              std::vector<std::string>* info) {
    return RawCommand(FB_CMD_FLASH ":" + partition, "Writing '" + partition + "'", response, info);
}

RetCode FastBootDriver::GetVar(const std::string& key, std::string* val,
                               std::vector<std::string>* info) {
    return RawCommand(FB_CMD_GETVAR ":" + key, val, info);
}

RetCode FastBootDriver::GetVarAll(std::vector<std::string>* response) {
    std::string tmp;
    return GetVar("all", &tmp, response);
}

RetCode FastBootDriver::Reboot(std::string* response, std::vector<std::string>* info) {
    return RawCommand(FB_CMD_REBOOT, "Rebooting", response, info);
}

RetCode FastBootDriver::RebootTo(std::string target, std::string* response,
                                 std::vector<std::string>* info) {
    return RawCommand("reboot-" + target, "Rebooting into " + target, response, info);
}

RetCode FastBootDriver::ResizePartition(const std::string& partition, const std::string& size) {
    return RawCommand(FB_CMD_RESIZE_PARTITION ":" + partition + ":" + size,
                      "Resizing '" + partition + "'");
}

RetCode FastBootDriver::SetActive(const std::string& slot, std::string* response,
                                  std::vector<std::string>* info) {
    return RawCommand(FB_CMD_SET_ACTIVE ":" + slot, "Setting current slot to '" + slot + "'",
                      response, info);
}

RetCode FastBootDriver::SnapshotUpdateCommand(const std::string& command, std::string* response,
                                              std::vector<std::string>* info) {
    prolog_(StringPrintf("Snapshot %s", command.c_str()));
    std::string raw = FB_CMD_SNAPSHOT_UPDATE ":" + command;
    auto result = RawCommand(raw, response, info);
    epilog_(result);
    return result;
}

RetCode FastBootDriver::FlashPartition(const std::string& partition,
                                       const std::vector<char>& data) {
    RetCode ret;
    if ((ret = Download(partition, data))) {
        return ret;
    }
    return Flash(partition);
}

RetCode FastBootDriver::FlashPartition(const std::string& partition, android::base::borrowed_fd fd,
                                       uint32_t size) {
    RetCode ret;
    if ((ret = Download(partition, fd, size))) {
        return ret;
    }
    return Flash(partition);
}

RetCode FastBootDriver::FlashPartition(const std::string& partition, sparse_file* s, uint32_t size,
                                       size_t current, size_t total) {
    RetCode ret;
    if ((ret = Download(partition, s, size, current, total, false))) {
        return ret;
    }
    return Flash(partition);
}

RetCode FastBootDriver::Partitions(std::vector<std::tuple<std::string, uint64_t>>* partitions) {
    std::vector<std::string> all;
    RetCode ret;
    if ((ret = GetVarAll(&all))) {
        return ret;
    }

    std::regex reg("partition-size[[:s:]]*:[[:s:]]*([[:w:]]+)[[:s:]]*:[[:s:]]*0x([[:xdigit:]]+)");
    std::smatch sm;

    for (auto& s : all) {
        if (std::regex_match(s, sm, reg)) {
            std::string m1(sm[1]);
            std::string m2(sm[2]);
            uint64_t tmp = strtoll(m2.c_str(), 0, 16);
            partitions->push_back(std::make_tuple(m1, tmp));
        }
    }
    return SUCCESS;
}

RetCode FastBootDriver::Download(const std::string& name, android::base::borrowed_fd fd,
                                 size_t size, std::string* response,
                                 std::vector<std::string>* info) {
    prolog_(StringPrintf("Sending '%s' (%zu KB)", name.c_str(), size / 1024));
    auto result = Download(fd, size, response, info);
    epilog_(result);
    return result;
}

RetCode FastBootDriver::Download(android::base::borrowed_fd fd, size_t size, std::string* response,
                                 std::vector<std::string>* info) {
    RetCode ret;

    if ((size <= 0 || size > MAX_DOWNLOAD_SIZE) && !disable_checks_) {
        error_ = "File is too large to download";
        return BAD_ARG;
    }

    uint32_t u32size = static_cast<uint32_t>(size);
    if ((ret = DownloadCommand(u32size, response, info))) {
        return ret;
    }

    // Write the buffer
    if ((ret = SendBuffer(fd, size))) {
        return ret;
    }

    // Wait for response
    return HandleResponse(response, info);
}

RetCode FastBootDriver::Download(const std::string& name, const std::vector<char>& buf,
                                 std::string* response, std::vector<std::string>* info) {
    prolog_(StringPrintf("Sending '%s' (%zu KB)", name.c_str(), buf.size() / 1024));
    auto result = Download(buf, response, info);
    epilog_(result);
    return result;
}

RetCode FastBootDriver::Download(const std::vector<char>& buf, std::string* response,
                                 std::vector<std::string>* info) {
    RetCode ret;
    error_ = "";
    if ((buf.size() == 0 || buf.size() > MAX_DOWNLOAD_SIZE) && !disable_checks_) {
        error_ = "Buffer is too large or 0 bytes";
        return BAD_ARG;
    }

    if ((ret = DownloadCommand(buf.size(), response, info))) {
        return ret;
    }

    // Write the buffer
    if ((ret = SendBuffer(buf))) {
        return ret;
    }

    // Wait for response
    return HandleResponse(response, info);
}

RetCode FastBootDriver::Download(const std::string& partition, struct sparse_file* s, uint32_t size,
                                 size_t current, size_t total, bool use_crc, std::string* response,
                                 std::vector<std::string>* info) {
    prolog_(StringPrintf("Sending sparse '%s' %zu/%zu (%u KB)", partition.c_str(), current, total,
                         size / 1024));
    auto result = Download(s, use_crc, response, info);
    epilog_(result);
    return result;
}

RetCode FastBootDriver::Download(sparse_file* s, bool use_crc, std::string* response,
                                 std::vector<std::string>* info) {
    error_ = "";
    int64_t size = sparse_file_len(s, true, use_crc);
    if (size <= 0 || size > MAX_DOWNLOAD_SIZE) {
        error_ = "Sparse file is too large or invalid";
        return BAD_ARG;
    }

    RetCode ret;
    uint32_t u32size = static_cast<uint32_t>(size);
    if ((ret = DownloadCommand(u32size, response, info))) {
        return ret;
    }

    struct SparseCBPrivate {
        FastBootDriver* self;
        std::vector<char> tpbuf;
    } cb_priv;
    cb_priv.self = this;

    auto cb = [](void* priv, const void* buf, size_t len) -> int {
        SparseCBPrivate* data = static_cast<SparseCBPrivate*>(priv);
        const char* cbuf = static_cast<const char*>(buf);
        return data->self->SparseWriteCallback(data->tpbuf, cbuf, len);
    };

    if (sparse_file_callback(s, true, use_crc, cb, &cb_priv) < 0) {
        error_ = "Error reading sparse file";
        return IO_ERROR;
    }

    // Now flush
    if (cb_priv.tpbuf.size() && (ret = SendBuffer(cb_priv.tpbuf))) {
        return ret;
    }

    return HandleResponse(response, info);
}

// Helpers
void FastBootDriver::SetInfoCallback(std::function<void(const std::string&)> info) {
    info_ = info;
}

const std::string FastBootDriver::RCString(RetCode rc) {
    switch (rc) {
        case SUCCESS:
            return std::string("Success");

        case BAD_ARG:
            return std::string("Invalid Argument");

        case IO_ERROR:
            return std::string("I/O Error");

        case BAD_DEV_RESP:
            return std::string("Invalid Device Response");

        case DEVICE_FAIL:
            return std::string("Device Error");

        case TIMEOUT:
            return std::string("Timeout");

        default:
            return std::string("Unknown Error");
    }
}

std::string FastBootDriver::Error() {
    return error_;
}

/****************************** PROTECTED *************************************/
RetCode FastBootDriver::RawCommand(const std::string& cmd, const std::string& message,
                                   std::string* response, std::vector<std::string>* info,
                                   int* dsize) {
    prolog_(message);
    auto result = RawCommand(cmd, response, info, dsize);
    epilog_(result);
    return result;
}

RetCode FastBootDriver::RawCommand(const std::string& cmd, std::string* response,
                                   std::vector<std::string>* info, int* dsize) {
    error_ = "";  // Clear any pending error
    if (cmd.size() > FB_COMMAND_SZ && !disable_checks_) {
        error_ = "Command length to RawCommand() is too long";
        return BAD_ARG;
    }

    device.ExecuteCommand((char *)cmd.c_str());

    // Read the response
    return HandleResponse(response, info, dsize);
}

RetCode FastBootDriver::DownloadCommand(uint32_t size, std::string* response,
                                        std::vector<std::string>* info) {
    std::string cmd(android::base::StringPrintf("%s:%08" PRIx32, FB_CMD_DOWNLOAD, size));
    RetCode ret;
    if ((ret = RawCommand(cmd, response, info))) {
        return ret;
    }
    return SUCCESS;
}

RetCode FastBootDriver::HandleResponse(std::string* response, std::vector<std::string>* info,
                                       int* dsize) {
    if (response) *response = std::move(device.get_message());
    if (info) *info = std::move(device.get_info());
    // TODO: Monitor device.info_ and fire info_ callback?
    if (device.get_result() == FastbootResult::OKAY) {
        return SUCCESS;
    } else if (device.get_result() == FastbootResult::FAIL) {
        error_ = android::base::StringPrintf("remote: '%s'", device.get_message().c_str());
        return DEVICE_FAIL;
    } else if (device.get_result() == FastbootResult::DATA) {
        uint32_t num = strtol(device.get_message().c_str(), 0, 16);
        if (num > MAX_DOWNLOAD_SIZE) {
            error_ = android::base::StringPrintf("Data size too large (%d)", num);
            return BAD_DEV_RESP;
        }
        if (dsize) *dsize = num;
        return SUCCESS;
    }
    return BAD_DEV_RESP;
}

std::string FastBootDriver::ErrnoStr(const std::string& msg) {
    return android::base::StringPrintf("%s (%s)", msg.c_str(), strerror(errno));
}

/******************************* PRIVATE **************************************/
RetCode FastBootDriver::SendBuffer(android::base::borrowed_fd fd, size_t size) {
    static constexpr uint32_t MAX_MAP_SIZE = 512 * 1024 * 1024;
    off64_t offset = 0;
    uint32_t remaining = size;
    RetCode ret;

    while (remaining) {
        // Memory map the file
        size_t len = std::min(remaining, MAX_MAP_SIZE);
        auto mapping{android::base::MappedFile::FromFd(fd, offset, len, PROT_READ)};
        if (!mapping) {
            error_ = "Creating filemap failed";
            return IO_ERROR;
        }

        if ((ret = SendBuffer(mapping->data(), mapping->size()))) {
            return ret;
        }

        remaining -= len;
        offset += len;
    }

    return SUCCESS;
}

RetCode FastBootDriver::SendBuffer(const std::vector<char>& buf) {
    // Write the buffer
    return SendBuffer(buf.data(), buf.size());
}

RetCode FastBootDriver::SendBuffer(const void* buf, size_t size) {
    // ioctl on 0-length buffer causes freezing
    if (!size) {
        return BAD_ARG;
    }
    // Write the buffer
    ssize_t tmp = device.StreamDownload(buf, size);

    if (tmp < 0) {
        error_ = ErrnoStr("Write to device failed in SendBuffer()");
        return IO_ERROR;
    } else if (static_cast<size_t>(tmp) != size) {
        error_ = android::base::StringPrintf("Failed to write all %zu bytes", size);

        return IO_ERROR;
    }

    return SUCCESS;
}

int FastBootDriver::SparseWriteCallback(std::vector<char>& tpbuf, const char* data, size_t len) {
    size_t total = 0;
    size_t to_write = std::min(TRANSPORT_CHUNK_SIZE - tpbuf.size(), len);

    // Handle the residual
    tpbuf.insert(tpbuf.end(), data, data + to_write);
    if (tpbuf.size() < TRANSPORT_CHUNK_SIZE) {  // Nothing enough to send rn
        return 0;
    }

    if (SendBuffer(tpbuf)) {
        error_ = ErrnoStr("Send failed in SparseWriteCallback()");
        return -1;
    }
    tpbuf.clear();
    total += to_write;

    // Now we need to send a multiple of chunk size
    size_t nchunks = (len - total) / TRANSPORT_CHUNK_SIZE;
    size_t nbytes = TRANSPORT_CHUNK_SIZE * nchunks;
    if (nbytes && SendBuffer(data + total, nbytes)) {  // Don't send a ZLP
        error_ = ErrnoStr("Send failed in SparseWriteCallback()");
        return -1;
    }
    total += nbytes;

    if (len - total > 0) {  // We have residual data to save for next time
        tpbuf.assign(data + total, data + len);
    }

    return 0;
}

}  // End namespace fastboot
