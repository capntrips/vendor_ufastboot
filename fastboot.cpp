/*
 * Copyright (C) 2008 The Android Open Source Project
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

#include "fastboot.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>

#include <chrono>
#include <functional>
#include <regex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <android-base/endian.h>
#include <android-base/file.h>
#include <android-base/macros.h>
#include <android-base/parseint.h>
#include <android-base/parsenetaddress.h>
#include <android-base/stringprintf.h>
#include <android-base/strings.h>
#include <android-base/unique_fd.h>
#include <libavb/libavb.h>
#include <liblp/liblp.h>
#include <sparse/sparse.h>
#include <ziparchive/zip_archive.h>

#include "bootimg_utils.h"
#include "constants.h"
#include "fastboot_driver.h"
#include "fs.h"
#include "util.h"
#include "vendor_boot_img_utils.h"

using android::base::borrowed_fd;
using android::base::ReadFully;
using android::base::Split;
using android::base::Trim;
using android::base::unique_fd;
using namespace std::string_literals;
using namespace std::placeholders;

// Don't resparse files in too-big chunks.
// libsparse will support INT_MAX, but this results in large allocations, so
// let's keep it at 1GB to avoid memory pressure on the host.
static constexpr int64_t RESPARSE_LIMIT = 1 * 1024 * 1024 * 1024;
static uint64_t sparse_limit = 0;
static int64_t target_sparse_limit = -1;

static std::string g_cmdline;
static std::string g_dtb_path;

static bool g_disable_verity = false;
static bool g_disable_verification = false;

static const std::string convert_fbe_marker_filename("convert_fbe");

fastboot::FastBootDriver* fb = nullptr;

enum fb_buffer_type {
    FB_BUFFER_FD,
    FB_BUFFER_SPARSE,
};

struct fastboot_buffer {
    enum fb_buffer_type type;
    void* data;
    int64_t sz;
    unique_fd fd;
    int64_t image_size;
};

enum class ImageType {
    // Must be flashed for device to boot into the kernel.
    BootCritical,
    // Normal partition to be flashed during "flashall".
    Normal,
    // Partition that is never flashed during "flashall".
    Extra
};

struct Image {
    const char* nickname;
    const char* img_name;
    const char* sig_name;
    const char* part_name;
    bool optional_if_no_image;
    ImageType type;
    bool IsSecondary() const { return nickname == nullptr; }
};

static Image images[] = {
        // clang-format off
    { "boot",     "boot.img",         "boot.sig",     "boot",     false, ImageType::BootCritical },
    { nullptr,    "boot_other.img",   "boot.sig",     "boot",     true,  ImageType::Normal },
    { "cache",    "cache.img",        "cache.sig",    "cache",    true,  ImageType::Extra },
    { "dtbo",     "dtbo.img",         "dtbo.sig",     "dtbo",     true,  ImageType::BootCritical },
    { "dts",      "dt.img",           "dt.sig",       "dts",      true,  ImageType::BootCritical },
    { "odm",      "odm.img",          "odm.sig",      "odm",      true,  ImageType::Normal },
    { "odm_dlkm", "odm_dlkm.img",     "odm_dlkm.sig", "odm_dlkm", true,  ImageType::Normal },
    { "product",  "product.img",      "product.sig",  "product",  true,  ImageType::Normal },
    { "pvmfw",    "pvmfw.img",        "pvmfw.sig",    "pvmfw",    true,  ImageType::BootCritical },
    { "recovery", "recovery.img",     "recovery.sig", "recovery", true,  ImageType::BootCritical },
    { "super",    "super.img",        "super.sig",    "super",    true,  ImageType::Extra },
    { "system",   "system.img",       "system.sig",   "system",   false, ImageType::Normal },
    { "system_ext",
                  "system_ext.img",   "system_ext.sig",
                                                      "system_ext",
                                                                  true,  ImageType::Normal },
    { nullptr,    "system_other.img", "system.sig",   "system",   true,  ImageType::Normal },
    { "userdata", "userdata.img",     "userdata.sig", "userdata", true,  ImageType::Extra },
    { "vbmeta",   "vbmeta.img",       "vbmeta.sig",   "vbmeta",   true,  ImageType::BootCritical },
    { "vbmeta_system",
                  "vbmeta_system.img",
                                      "vbmeta_system.sig",
                                                      "vbmeta_system",
                                                                  true,  ImageType::BootCritical },
    { "vbmeta_vendor",
                  "vbmeta_vendor.img",
                                      "vbmeta_vendor.sig",
                                                      "vbmeta_vendor",
                                                                  true,  ImageType::BootCritical },
    { "vendor",   "vendor.img",       "vendor.sig",   "vendor",   true,  ImageType::Normal },
    { "vendor_boot",
                  "vendor_boot.img",  "vendor_boot.sig",
                                                      "vendor_boot",
                                                                  true,  ImageType::BootCritical },
    { "vendor_dlkm",
                  "vendor_dlkm.img",  "vendor_dlkm.sig",
                                                      "vendor_dlkm",
                                                                  true,  ImageType::Normal },
    { nullptr,    "vendor_other.img", "vendor.sig",   "vendor",   true,  ImageType::Normal },
        // clang-format on
};

static char* get_android_product_out() {
    char* dir = getenv("ANDROID_PRODUCT_OUT");
    if (dir == nullptr || dir[0] == '\0') {
        return nullptr;
    }
    return dir;
}

static std::string find_item_given_name(const std::string& img_name) {
    char* dir = get_android_product_out();
    if (!dir) {
        die("ANDROID_PRODUCT_OUT not set");
    }
    return std::string(dir) + "/" + img_name;
}

static std::string find_item(const std::string& item) {
    for (size_t i = 0; i < arraysize(images); ++i) {
        if (images[i].nickname && item == images[i].nickname) {
            return find_item_given_name(images[i].img_name);
        }
    }

    fprintf(stderr, "unknown partition '%s'\n", item.c_str());
    return "";
}

double last_start_time;

static void Status(const std::string& message) {
    if (!message.empty()) {
        static constexpr char kStatusFormat[] = "%-50s ";
        fprintf(stderr, kStatusFormat, message.c_str());
    }
    last_start_time = now();
}

static void Epilog(int status) {
    if (status) {
        fprintf(stderr, "FAILED (%s)\n", fb->Error().c_str());
        die("Command failed");
    } else {
        double split = now();
        fprintf(stderr, "OKAY [%7.3fs]\n", (split - last_start_time));
    }
}

static void InfoMessage(const std::string& info) {
    fprintf(stderr, "(bootloader) %s\n", info.c_str());
}

static int64_t get_file_size(borrowed_fd fd) {
    struct stat sb;
    if (fstat(fd.get(), &sb) == -1) {
        die("could not get file size");
    }
    return sb.st_size;
}

static void syntax_error(const char* fmt, ...) {
    fprintf(stderr, "fastboot: usage: ");

    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);

    fprintf(stderr, "\n");
    exit(1);
}

static int show_help() {
    // clang-format off
    fprintf(stdout,
//                    1         2         3         4         5         6         7         8
//           12345678901234567890123456789012345678901234567890123456789012345678901234567890
            "usage: fastboot [OPTION...] COMMAND...\n"
            "\n"
            "flashing:\n"
            " flash PARTITION [FILENAME] Flash given partition, using the image from\n"
            "                            $ANDROID_PRODUCT_OUT if no filename is given.\n"
            "\n"
            "basics:\n"
            " getvar NAME                Display given bootloader variable.\n"
            " reboot [bootloader]        Reboot device.\n"
            "\n"
            "advanced:\n"
            " set_active SLOT            Set the active slot.\n"
            " wipe-super [SUPER_EMPTY]   Wipe the super partition. This will reset it to\n"
            "                            contain an empty set of default dynamic partitions.\n"
            " create-logical-partition NAME SIZE\n"
            "                            Create a logical partition with the given name and\n"
            "                            size, in the super partition.\n"
            " delete-logical-partition NAME\n"
            "                            Delete a logical partition with the given name.\n"
            " resize-logical-partition NAME SIZE\n"
            "                            Change the size of the named logical partition.\n"
            "\n"
            "options:\n"
            " --slot SLOT                Use SLOT; 'all' for both slots, 'other' for\n"
            "                            non-current slot (default: current active slot).\n"
            " --set-active[=SLOT]        Sets the active slot before rebooting.\n"
            " --verbose, -v              Verbose output.\n"
            " --version                  Display version.\n"
            " --help, -h                 Show this message.\n"
        );
    // clang-format on
    return 0;
}

static std::string make_temporary_template() {
    const char* tmpdir = getenv("TMPDIR");
    if (tmpdir == nullptr) tmpdir = P_tmpdir;
    return std::string(tmpdir) + "/fastboot_userdata_XXXXXX";
}

static int make_temporary_fd(const char* what) {
    std::string path_template(make_temporary_template());
    int fd = mkstemp(&path_template[0]);
    if (fd == -1) {
        die("failed to create temporary file for %s with template %s: %s\n",
            path_template.c_str(), what, strerror(errno));
    }
    unlink(path_template.c_str());
    return fd;
}

static void DisplayVarOrError(const std::string& label, const std::string& var) {
    std::string value;

    if (fb->GetVar(var, &value) != fastboot::SUCCESS) {
        Status("getvar:" + var);
        fprintf(stderr, "FAILED (%s)\n", fb->Error().c_str());
        return;
    }
    fprintf(stderr, "%s: %s\n", label.c_str(), value.c_str());
}

static struct sparse_file** load_sparse_files(int fd, int64_t max_size) {
    struct sparse_file* s = sparse_file_import_auto(fd, false, true);
    if (!s) die("cannot sparse read file");

    if (max_size <= 0 || max_size > std::numeric_limits<uint32_t>::max()) {
      die("invalid max size %" PRId64, max_size);
    }

    int files = sparse_file_resparse(s, max_size, nullptr, 0);
    if (files < 0) die("Failed to resparse");

    sparse_file** out_s = reinterpret_cast<sparse_file**>(calloc(sizeof(struct sparse_file *), files + 1));
    if (!out_s) die("Failed to allocate sparse file array");

    files = sparse_file_resparse(s, max_size, out_s, files);
    if (files < 0) die("Failed to resparse");

    return out_s;
}

static uint64_t get_uint_var(const char* var_name) {
    std::string value_str;
    if (fb->GetVar(var_name, &value_str) != fastboot::SUCCESS || value_str.empty()) {
        verbose("target didn't report %s", var_name);
        return 0;
    }

    // Some bootloaders (angler, for example) send spurious whitespace too.
    value_str = android::base::Trim(value_str);

    uint64_t value;
    if (!android::base::ParseUint(value_str, &value)) {
        fprintf(stderr, "couldn't parse %s '%s'\n", var_name, value_str.c_str());
        return 0;
    }
    if (value > 0) verbose("target reported %s of %" PRId64 " bytes", var_name, value);
    return value;
}

static int64_t get_sparse_limit(int64_t size) {
    int64_t limit = sparse_limit;
    if (limit == 0) {
        // Unlimited, so see what the target device's limit is.
        // TODO: shouldn't we apply this limit even if you've used -S?
        if (target_sparse_limit == -1) {
            target_sparse_limit = static_cast<int64_t>(get_uint_var("max-download-size"));
        }
        if (target_sparse_limit > 0) {
            limit = target_sparse_limit;
        } else {
            return 0;
        }
    }

    if (size > limit) {
        return std::min(limit, RESPARSE_LIMIT);
    }

    return 0;
}

static bool load_buf_fd(unique_fd fd, struct fastboot_buffer* buf) {
    int64_t sz = get_file_size(fd);
    if (sz == -1) {
        return false;
    }

    if (sparse_file* s = sparse_file_import(fd.get(), false, false)) {
        buf->image_size = sparse_file_len(s, false, false);
        sparse_file_destroy(s);
    } else {
        buf->image_size = sz;
    }

    lseek(fd.get(), 0, SEEK_SET);
    int64_t limit = get_sparse_limit(sz);
    buf->fd = std::move(fd);
    if (limit) {
        sparse_file** s = load_sparse_files(buf->fd.get(), limit);
        if (s == nullptr) {
            return false;
        }
        buf->type = FB_BUFFER_SPARSE;
        buf->data = s;
    } else {
        buf->type = FB_BUFFER_FD;
        buf->data = nullptr;
        buf->sz = sz;
    }

    return true;
}

static bool load_buf(const char* fname, struct fastboot_buffer* buf) {
    unique_fd fd(TEMP_FAILURE_RETRY(open(fname, O_RDONLY | O_BINARY)));

    if (fd == -1) {
        return false;
    }

    struct stat s;
    if (fstat(fd.get(), &s)) {
        return false;
    }
    if (!S_ISREG(s.st_mode)) {
        errno = S_ISDIR(s.st_mode) ? EISDIR : EINVAL;
        return false;
    }

    return load_buf_fd(std::move(fd), buf);
}

static void rewrite_vbmeta_buffer(struct fastboot_buffer* buf, bool vbmeta_in_boot) {
    // Buffer needs to be at least the size of the VBMeta struct which
    // is 256 bytes.
    if (buf->sz < 256) {
        return;
    }

    std::string data;
    if (!android::base::ReadFdToString(buf->fd, &data)) {
        die("Failed reading from vbmeta");
    }

    uint64_t vbmeta_offset = 0;
    if (vbmeta_in_boot) {
        // Tries to locate top-level vbmeta from boot.img footer.
        uint64_t footer_offset = buf->sz - AVB_FOOTER_SIZE;
        if (0 != data.compare(footer_offset, AVB_FOOTER_MAGIC_LEN, AVB_FOOTER_MAGIC)) {
            die("Failed to find AVB_FOOTER at offset: %" PRId64, footer_offset);
        }
        const AvbFooter* footer = reinterpret_cast<const AvbFooter*>(data.c_str() + footer_offset);
        vbmeta_offset = be64toh(footer->vbmeta_offset);
    }
    // Ensures there is AVB_MAGIC at vbmeta_offset.
    if (0 != data.compare(vbmeta_offset, AVB_MAGIC_LEN, AVB_MAGIC)) {
        die("Failed to find AVB_MAGIC at offset: %" PRId64, vbmeta_offset);
    }

    fprintf(stderr, "Rewriting vbmeta struct at offset: %" PRId64 "\n", vbmeta_offset);

    // There's a 32-bit big endian |flags| field at offset 120 where
    // bit 0 corresponds to disable-verity and bit 1 corresponds to
    // disable-verification.
    //
    // See external/avb/libavb/avb_vbmeta_image.h for the layout of
    // the VBMeta struct.
    uint64_t flags_offset = 123 + vbmeta_offset;
    if (g_disable_verity) {
        data[flags_offset] |= 0x01;
    }
    if (g_disable_verification) {
        data[flags_offset] |= 0x02;
    }

    unique_fd fd(make_temporary_fd("vbmeta rewriting"));
    if (!android::base::WriteStringToFd(data, fd)) {
        die("Failed writing to modified vbmeta");
    }
    buf->fd = std::move(fd);
    lseek(buf->fd.get(), 0, SEEK_SET);
}

static bool has_vbmeta_partition() {
    std::string partition_type;
    return fb->GetVar("partition-type:vbmeta", &partition_type) == fastboot::SUCCESS ||
           fb->GetVar("partition-type:vbmeta_a", &partition_type) == fastboot::SUCCESS ||
           fb->GetVar("partition-type:vbmeta_b", &partition_type) == fastboot::SUCCESS;
}

static bool is_logical(const std::string& partition) {
    std::string value;
    return fb->GetVar("is-logical:" + partition, &value) == fastboot::SUCCESS && value == "yes";
}

static std::string fb_fix_numeric_var(std::string var) {
    // Some bootloaders (angler, for example), send spurious leading whitespace.
    var = android::base::Trim(var);
    // Some bootloaders (hammerhead, for example) use implicit hex.
    // This code used to use strtol with base 16.
    if (!android::base::StartsWith(var, "0x")) var = "0x" + var;
    return var;
}

static uint64_t get_partition_size(const std::string& partition) {
    std::string partition_size_str;
    if (fb->GetVar("partition-size:" + partition, &partition_size_str) != fastboot::SUCCESS) {
        if (!is_logical(partition)) {
            return 0;
        }
        die("cannot get partition size for %s", partition.c_str());
    }

    partition_size_str = fb_fix_numeric_var(partition_size_str);
    uint64_t partition_size;
    if (!android::base::ParseUint(partition_size_str, &partition_size)) {
        if (!is_logical(partition)) {
            return 0;
        }
        die("Couldn't parse partition size '%s'.", partition_size_str.c_str());
    }
    return partition_size;
}

static void copy_boot_avb_footer(const std::string& partition, struct fastboot_buffer* buf) {
    if (buf->sz < AVB_FOOTER_SIZE) {
        return;
    }

    std::string data;
    if (!android::base::ReadFdToString(buf->fd, &data)) {
        die("Failed reading from boot");
    }

    uint64_t footer_offset = buf->sz - AVB_FOOTER_SIZE;
    if (0 != data.compare(footer_offset, AVB_FOOTER_MAGIC_LEN, AVB_FOOTER_MAGIC)) {
        return;
    }
    // If overflows and negative, it should be < buf->sz.
    int64_t partition_size = static_cast<int64_t>(get_partition_size(partition));

    if (partition_size == buf->sz) {
        return;
    }
    if (partition_size < buf->sz) {
        die("boot partition is smaller than boot image");
    }

    unique_fd fd(make_temporary_fd("boot rewriting"));
    if (!android::base::WriteStringToFd(data, fd)) {
        die("Failed writing to modified boot");
    }
    lseek(fd.get(), partition_size - AVB_FOOTER_SIZE, SEEK_SET);
    if (!android::base::WriteStringToFd(data.substr(footer_offset), fd)) {
        die("Failed copying AVB footer in boot");
    }
    buf->fd = std::move(fd);
    buf->sz = partition_size;
    lseek(buf->fd.get(), 0, SEEK_SET);
}

static void flash_buf(const std::string& partition, struct fastboot_buffer *buf)
{
    sparse_file** s;

    if (partition == "boot" || partition == "boot_a" || partition == "boot_b") {
        copy_boot_avb_footer(partition, buf);
    }

    // Rewrite vbmeta if that's what we're flashing and modification has been requested.
    if (g_disable_verity || g_disable_verification) {
        if (partition == "vbmeta" || partition == "vbmeta_a" || partition == "vbmeta_b") {
            rewrite_vbmeta_buffer(buf, false /* vbmeta_in_boot */);
        } else if (!has_vbmeta_partition() &&
                   (partition == "boot" || partition == "boot_a" || partition == "boot_b")) {
            rewrite_vbmeta_buffer(buf, true /* vbmeta_in_boot */ );
        }
    }

    switch (buf->type) {
        case FB_BUFFER_SPARSE: {
            std::vector<std::pair<sparse_file*, int64_t>> sparse_files;
            s = reinterpret_cast<sparse_file**>(buf->data);
            while (*s) {
                int64_t sz = sparse_file_len(*s, true, false);
                sparse_files.emplace_back(*s, sz);
                ++s;
            }

            for (size_t i = 0; i < sparse_files.size(); ++i) {
                const auto& pair = sparse_files[i];
                fb->FlashPartition(partition, pair.first, pair.second, i + 1, sparse_files.size());
            }
            break;
        }
        case FB_BUFFER_FD:
            fb->FlashPartition(partition, buf->fd, buf->sz);
            break;
        default:
            die("unknown buffer type: %d", buf->type);
    }
}

static std::string get_current_slot() {
    std::string current_slot;
    if (fb->GetVar("current-slot", &current_slot) != fastboot::SUCCESS) return "";
    if (current_slot[0] == '_') current_slot.erase(0, 1);
    return current_slot;
}

static int get_slot_count() {
    std::string var;
    int count = 0;
    if (fb->GetVar("slot-count", &var) != fastboot::SUCCESS ||
        !android::base::ParseInt(var, &count)) {
        return 0;
    }
    return count;
}

// Given a current slot, this returns what the 'other' slot is.
static std::string get_other_slot(const std::string& current_slot, int count) {
    if (count == 0) return "";

    char next = (current_slot[0] - 'a' + 1)%count + 'a';
    return std::string(1, next);
}

static std::string get_other_slot(int count) {
    return get_other_slot(get_current_slot(), count);
}

static std::string verify_slot(const std::string& slot_name, bool allow_all) {
    std::string slot = slot_name;
    if (slot == "all") {
        if (allow_all) {
            return "all";
        } else {
            int count = get_slot_count();
            if (count > 0) {
                return "a";
            } else {
                die("No known slots");
            }
        }
    }

    int count = get_slot_count();
    if (count == 0) die("Device does not support slots");

    if (slot == "other") {
        std::string other = get_other_slot( count);
        if (other == "") {
           die("No known slots");
        }
        return other;
    }

    if (slot.size() == 1 && (slot[0]-'a' >= 0 && slot[0]-'a' < count)) return slot;

    fprintf(stderr, "Slot %s does not exist. supported slots are:\n", slot.c_str());
    for (int i=0; i<count; i++) {
        fprintf(stderr, "%c\n", (char)(i + 'a'));
    }

    exit(1);
}

static std::string verify_slot(const std::string& slot) {
   return verify_slot(slot, true);
}

static void do_for_partition(const std::string& part, const std::string& slot,
                             const std::function<void(const std::string&)>& func, bool force_slot) {
    std::string has_slot;
    std::string current_slot;
    // |part| can be vendor_boot:default. Append slot to the first token.
    auto part_tokens = android::base::Split(part, ":");

    if (fb->GetVar("has-slot:" + part_tokens[0], &has_slot) != fastboot::SUCCESS) {
        /* If has-slot is not supported, the answer is no. */
        has_slot = "no";
    }
    if (has_slot == "yes") {
        if (slot == "") {
            current_slot = get_current_slot();
            if (current_slot == "") {
                die("Failed to identify current slot");
            }
            part_tokens[0] += "_" + current_slot;
        } else {
            part_tokens[0] += "_" + slot;
        }
        func(android::base::Join(part_tokens, ":"));
    } else {
        if (force_slot && slot != "") {
            fprintf(stderr, "Warning: %s does not support slots, and slot %s was requested.\n",
                    part_tokens[0].c_str(), slot.c_str());
        }
        func(part);
    }
}

/* This function will find the real partition name given a base name, and a slot. If slot is NULL or
 * empty, it will use the current slot. If slot is "all", it will return a list of all possible
 * partition names. If force_slot is true, it will fail if a slot is specified, and the given
 * partition does not support slots.
 */
static void do_for_partitions(const std::string& part, const std::string& slot,
                              const std::function<void(const std::string&)>& func, bool force_slot) {
    std::string has_slot;
    // |part| can be vendor_boot:default. Query has-slot on the first token only.
    auto part_tokens = android::base::Split(part, ":");

    if (slot == "all") {
        if (fb->GetVar("has-slot:" + part_tokens[0], &has_slot) != fastboot::SUCCESS) {
            die("Could not check if partition %s has slot %s", part_tokens[0].c_str(),
                slot.c_str());
        }
        if (has_slot == "yes") {
            for (int i=0; i < get_slot_count(); i++) {
                do_for_partition(part, std::string(1, (char)(i + 'a')), func, force_slot);
            }
        } else {
            do_for_partition(part, "", func, force_slot);
        }
    } else {
        do_for_partition(part, slot, func, force_slot);
    }
}

// Return immediately if not flashing a vendor boot image. If flashing a vendor boot image,
// repack vendor_boot image with an updated ramdisk. After execution, buf is set
// to the new image to flash, and return value is the real partition name to flash.
static std::string repack_ramdisk(const char* pname, struct fastboot_buffer* /*buf*/) {
    std::string_view pname_sv{pname};

    if (!android::base::StartsWith(pname_sv, "vendor_boot:") &&
        !android::base::StartsWith(pname_sv, "vendor_boot_a:") &&
        !android::base::StartsWith(pname_sv, "vendor_boot_b:")) {
        return std::string(pname_sv);
    }
    // TODO: Implement fetch?
    die("Fetch is not yet implemented.");
}

static void do_flash(const char* pname, const char* fname) {
    verbose("Do flash %s %s", pname, fname);
    struct fastboot_buffer buf;

    if (!load_buf(fname, &buf)) {
        die("cannot load '%s': %s", fname, strerror(errno));
    }
    if (is_logical(pname)) {
        fb->ResizePartition(pname, std::to_string(buf.image_size));
    }
    std::string flash_pname = repack_ramdisk(pname, &buf);
    flash_buf(flash_pname, &buf);
}

static std::string next_arg(std::vector<std::string>* args) {
    if (args->empty()) syntax_error("expected argument");
    std::string result = args->front();
    args->erase(args->begin());
    return result;
}

static bool wipe_super(const android::fs_mgr::LpMetadata& metadata, const std::string& slot,
                       std::string* message) {
    auto super_device = GetMetadataSuperBlockDevice(metadata);
    auto block_size = metadata.geometry.logical_block_size;
    auto super_bdev_name = android::fs_mgr::GetBlockDevicePartitionName(*super_device);

    if (super_bdev_name != "super") {
        // retrofit devices do not allow flashing to the retrofit partitions,
        // so enable it if we can.
        fb->RawCommand("oem allow-flash-super");
    }

    // Note: do not use die() in here, since we want TemporaryDir's destructor
    // to be called.
    TemporaryDir temp_dir;

    bool ok;
    if (metadata.block_devices.size() > 1) {
        ok = WriteSplitImageFiles(temp_dir.path, metadata, block_size, {}, true);
    } else {
        auto image_path = temp_dir.path + "/"s + super_bdev_name + ".img";
        ok = WriteToImageFile(image_path, metadata, block_size, {}, true);
    }
    if (!ok) {
        *message = "Could not generate a flashable super image file";
        return false;
    }

    for (const auto& block_device : metadata.block_devices) {
        auto partition = android::fs_mgr::GetBlockDevicePartitionName(block_device);
        bool force_slot = !!(block_device.flags & LP_BLOCK_DEVICE_SLOT_SUFFIXED);

        std::string image_name;
        if (metadata.block_devices.size() > 1) {
            image_name = "super_" + partition + ".img";
        } else {
            image_name = partition + ".img";
        }

        auto image_path = temp_dir.path + "/"s + image_name;
        auto flash = [&](const std::string& partition_name) {
            do_flash(partition_name.c_str(), image_path.c_str());
        };
        do_for_partitions(partition, slot, flash, force_slot);

        unlink(image_path.c_str());
    }
    return true;
}

static void do_wipe_super(const std::string& image, const std::string& slot_override) {
    if (access(image.c_str(), R_OK) != 0) {
        die("Could not read image: %s", image.c_str());
    }
    auto metadata = android::fs_mgr::ReadFromImageFile(image);
    if (!metadata) {
        die("Could not parse image: %s", image.c_str());
    }

    auto slot = slot_override;
    if (slot.empty()) {
        slot = get_current_slot();
    }

    std::string message;
    if (!wipe_super(*metadata.get(), slot, &message)) {
        die(message);
    }
}

int FastBootTool::Main(int argc, char* argv[]) {
    bool wants_reboot = false;
    bool wants_reboot_bootloader = false;
    bool wants_reboot_recovery = false;
    bool wants_reboot_fastboot = false;
    bool wants_set_active = false;
    int longindex;
    std::string slot_override;
    std::string next_active;

    const struct option longopts[] = {
        {"help", no_argument, 0, 'h'},
        {"set-active", optional_argument, 0, 'a'},
        {"slot", required_argument, 0, 0},
        {"verbose", no_argument, 0, 'v'},
        {"version", no_argument, 0, 0},
        {0, 0, 0, 0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "a::hls:S:vw", longopts, &longindex)) != -1) {
        if (c == 0) {
            std::string name{longopts[longindex].name};
            if (name == "slot") {
                slot_override = optarg;
            } else if (name == "version") {
                fprintf(stdout, "ufastboot version android-12.1.0_r4-alpha01\n");
                fprintf(stdout, "Installed as %s\n", android::base::GetExecutablePath().c_str());
                return 0;
            } else {
                die("unknown option %s", longopts[longindex].name);
            }
        } else {
            switch (c) {
                case 'a':
                    wants_set_active = true;
                    if (optarg) next_active = optarg;
                    break;
                case 'h':
                    return show_help();
                case 'v':
                    set_verbose();
                    break;
                case '?':
                    return 1;
                default:
                    abort();
            }
        }
    }

    argc -= optind;
    argv += optind;

    if (argc == 0 && !wants_set_active) syntax_error("no command");

    if (argc > 0 && !strcmp(*argv, "help")) {
        return show_help();
    }

    fastboot::DriverCallbacks driver_callbacks = {
        .prolog = Status,
        .epilog = Epilog,
        .info = InfoMessage,
    };
    fastboot::FastBootDriver fastboot_driver(driver_callbacks, false);
    fb = &fastboot_driver;

    const double start = now();

    if (slot_override != "") slot_override = verify_slot(slot_override);
    if (next_active != "") next_active = verify_slot(next_active, false);

    if (wants_set_active) {
        if (next_active == "") {
            if (slot_override == "") {
                std::string current_slot;
                if (fb->GetVar("current-slot", &current_slot) == fastboot::SUCCESS) {
                    if (current_slot[0] == '_') current_slot.erase(0, 1);
                    next_active = verify_slot(current_slot, false);
                } else {
                    wants_set_active = false;
                }
            } else {
                next_active = verify_slot(slot_override, false);
            }
        }
    }

    std::vector<std::string> args(argv, argv + argc);
    while (!args.empty()) {
        std::string command = next_arg(&args);

        if (command == FB_CMD_GETVAR) {
            std::string variable = next_arg(&args);
            DisplayVarOrError(variable, variable);
        } else if (command == FB_CMD_REBOOT) {
            wants_reboot = true;

            if (args.size() == 1) {
                std::string what = next_arg(&args);
                if (what == "bootloader") {
                    wants_reboot = false;
                    wants_reboot_bootloader = true;
                } else if (what == "recovery") {
                    wants_reboot = false;
                    wants_reboot_recovery = true;
                } else if (what == "fastboot") {
                    wants_reboot = false;
                    wants_reboot_fastboot = true;
                } else {
                    syntax_error("unknown reboot target %s", what.c_str());
                }

            }
            if (!args.empty()) syntax_error("junk after reboot command");
        } else if (command == FB_CMD_REBOOT_BOOTLOADER) {
            wants_reboot_bootloader = true;
        } else if (command == FB_CMD_REBOOT_RECOVERY) {
            wants_reboot_recovery = true;
        } else if (command == FB_CMD_REBOOT_FASTBOOT) {
            wants_reboot_fastboot = true;
        } else if (command == FB_CMD_FLASH) {
            std::string pname = next_arg(&args);

            std::string fname;
            if (!args.empty()) {
                fname = next_arg(&args);
            } else {
                fname = find_item(pname);
            }
            if (fname.empty()) die("cannot determine image filename for '%s'", pname.c_str());

            auto flash = [&](const std::string &partition) {
                do_flash(partition.c_str(), fname.c_str());
            };
            do_for_partitions(pname, slot_override, flash, true);
        } else if (command == FB_CMD_SET_ACTIVE) {
            std::string slot = verify_slot(next_arg(&args), false);
            fb->SetActive(slot);
        } else if (command == FB_CMD_CREATE_PARTITION) {
            std::string partition = next_arg(&args);
            std::string size = next_arg(&args);
            fb->CreatePartition(partition, size);
        } else if (command == FB_CMD_DELETE_PARTITION) {
            std::string partition = next_arg(&args);
            fb->DeletePartition(partition);
        } else if (command == FB_CMD_RESIZE_PARTITION) {
            std::string partition = next_arg(&args);
            std::string size = next_arg(&args);
            fb->ResizePartition(partition, size);
        } else if (command == "wipe-super") {
            std::string image;
            if (args.empty()) {
                image = find_item_given_name("super_empty.img");
            } else {
                image = next_arg(&args);
            }
            do_wipe_super(image, slot_override);
        } else {
            syntax_error("unknown command %s", command.c_str());
        }
    }

    if (wants_set_active) {
        fb->SetActive(next_active);
    }
    if (wants_reboot) {
        fb->Reboot();
    } else if (wants_reboot_bootloader) {
        fb->RebootTo("bootloader");
    } else if (wants_reboot_recovery) {
        fb->RebootTo("recovery");
    } else if (wants_reboot_fastboot) {
        fb->RebootTo("fastboot");
    }

    fprintf(stderr, "Finished. Total time: %.3fs\n", (now() - start));

    return 0;
}
