// Copyright 2018 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "garnet/bin/guest/guest_config.h"

#include <libgen.h>
#include <unistd.h>
#include <iostream>

#include "lib/fxl/command_line.h"
#include "lib/fxl/logging.h"
#include "lib/fxl/strings/string_number_conversions.h"
#include "third_party/rapidjson/rapidjson/document.h"

static void print_usage(fxl::CommandLine& cl) {
  // clang-format off
  std::cerr << "usage: " << cl.argv0() << " [OPTIONS]\n";
  std::cerr << "\n";
  std::cerr << "OPTIONS:\n";
  std::cerr << "\t--kernel=[kernel.bin]           Use file 'kernel.bin' as the kernel\n";
  std::cerr << "\t--ramdisk=[ramdisk.bin]         Use file 'ramdisk.bin' as a ramdisk\n";
  std::cerr << "\t--block=[block_spec]            Adds a block device with the given parameters\n";
  std::cerr << "\t--cmdline=[cmdline]             Use string 'cmdline' as the kernel command line\n";
  std::cerr << "\t--nographic                     Disable GPU device and graphics output\n";
  std::cerr << "\t--balloon-interval=[seconds]    Poll the virtio-balloon device every 'seconds' seconds\n";
  std::cerr << "\t                                and adjust the balloon size based on the amount of\n";
  std::cerr << "\t                                unused guest memory\n";
  std::cerr << "\t--balloon-threshold=[pages]     Number of unused pages to allow the guest to\n";
  std::cerr << "\t                                retain. Has no effect unless -m is also used\n";
  std::cerr << "\t--balloon-demand-page           Demand-page balloon deflate requests\n";
  std::cerr << "\n";
  std::cerr << "BLOCK SPEC\n";
  std::cerr << "\n";
  std::cerr << " Block devices can be specified by path:\n";
  std::cerr << "    /pkg/data/disk.img\n";
  std::cerr << "\n";
  std::cerr << " Additional Options:\n";
  std::cerr << "    rw/ro: Create a read/write or read-only device.\n";
  std::cerr << "    fdio:  Use the FDIO back-end for the block device.\n";
  std::cerr << "    fifo:  Use the FIFO back-end for the block device. This only works if the\n";
  std::cerr << "           target is a real block device (/dev/class/block/XYZ).\n";
  std::cerr << "\n";
  std::cerr << " Ex:\n";
  std::cerr << "\n";
  std::cerr << "  To open a filesystem resource packaged with the guest application\n";
  std::cerr << "  (read-only is important here as the /pkg/data namespace provides\n";
  std::cerr << "  read-only view into the package resources):\n";
  std::cerr << "\n";
  std::cerr << "      /pkg/data/system.img,fdio,ro\n";
  std::cerr << "\n";
  std::cerr << "  To specify a block device with a given path, read-write\n";
  std::cerr << "  permissions and the faster FIFO back-end:\n";
  std::cerr << "\n";
  std::cerr << "      /dev/class/block/000,fifo,rw\n";
  std::cerr << "\n";
  // clang-format on
}

static GuestConfigParser::OptionHandler save_option(std::string* out) {
  return [out](const std::string& key, const std::string& value) {
    if (value.empty()) {
      FXL_LOG(ERROR) << "Option: '" << key << "' expects a value (--" << key
                     << "=<value>)";
      return ZX_ERR_INVALID_ARGS;
    }
    *out = value;
    return ZX_OK;
  };
}

// A function that converts a string option into a custom type.
template <typename T>
using OptionTransform =
    std::function<zx_status_t(const std::string& arg, T* out)>;

// Handles and option by transforming the value and appending it to the
// given vector.
template <typename T>
static GuestConfigParser::OptionHandler append_option(
    std::vector<T>* out,
    OptionTransform<T> transform) {
  return [out, transform](const std::string& key, const std::string& value) {
    if (value.empty()) {
      FXL_LOG(ERROR) << "Option: '" << key << "' expects a value (--" << key
                     << "=<value>)";
      return ZX_ERR_INVALID_ARGS;
    }
    T t;
    zx_status_t status = transform(value, &t);
    if (status != ZX_OK) {
      FXL_LOG(ERROR) << "Failed to parse option string '" << value << "'";
      return status;
    }
    out->push_back(t);
    return ZX_OK;
  };
}

template <typename NumberType>
static GuestConfigParser::OptionHandler parse_number(NumberType* out) {
  return [out](const std::string& key, const std::string& value) {
    if (value.empty()) {
      FXL_LOG(ERROR) << "Option: '" << key << "' expects a value (--" << key
                     << "=<value>)";
      return ZX_ERR_INVALID_ARGS;
    }
    if (!fxl::StringToNumberWithError(value, out)) {
      FXL_LOG(ERROR) << "Unable to convert '" << value << "' into a number";
      return ZX_ERR_INVALID_ARGS;
    }
    return ZX_OK;
  };
}

// Create an |OptionHandler| that sets |out| to a boolean flag. This can be
// specified not only as '--foo=true' or '--foo=false', but also as '--foo', in
// which case |out| will take the value of |default_flag_value|.
static GuestConfigParser::OptionHandler set_flag(bool* out,
                                                 bool default_flag_value) {
  return [out, default_flag_value](const std::string& key,
                                   const std::string& option_value) {
    bool flag_value = default_flag_value;
    if (!option_value.empty()) {
      if (option_value == "true") {
        flag_value = default_flag_value;
      } else if (option_value == "false") {
        flag_value = !default_flag_value;
      } else {
        FXL_LOG(ERROR) << "Option: '" << key
                       << "' expects either 'true' or 'false'; received '"
                       << option_value << "'";

        return ZX_ERR_INVALID_ARGS;
      }
    }
    *out = flag_value;
    return ZX_OK;
  };
}

static zx_status_t parse_block_spec(const std::string& spec, BlockSpec* out) {
  std::string token;
  std::istringstream tokenStream(spec);
  while (std::getline(tokenStream, token, ',')) {
    if (token == "fdio") {
      out->data_plane = machina::BlockDispatcher::DataPlane::FDIO;
    } else if (token == "fifo") {
      out->data_plane = machina::BlockDispatcher::DataPlane::FIFO;
    } else if (token == "rw") {
      out->mode = machina::BlockDispatcher::Mode::RW;
    } else if (token == "ro") {
      out->mode = machina::BlockDispatcher::Mode::RO;
    } else if (token.size() > 0 && token[0] == '/') {
      out->path = std::move(token);
    }
  }
  return ZX_OK;
}

GuestConfig::GuestConfig()
    : kernel_path_("/pkg/data/kernel"), ramdisk_path_("/pkg/data/ramdisk") {}

GuestConfig::~GuestConfig() = default;

GuestConfigParser::GuestConfigParser(GuestConfig* config)
    : config_(config),
      options_{
          {"kernel", save_option(&config_->kernel_path_)},
          {"ramdisk", save_option(&config_->ramdisk_path_)},
          {"block",
           append_option<BlockSpec>(&config_->block_specs_, parse_block_spec)},
          {"cmdline", save_option(&config_->cmdline_)},
          {"balloon-demand-page",
           set_flag(&config_->balloon_demand_page_, true)},
          {"balloon-interval",
           parse_number(&config_->balloon_interval_seconds_)},
          {"balloon-threshold",
           parse_number(&config_->balloon_pages_threshold_)},
          {"nographic", set_flag(&config_->enable_gpu_, false)},
      } {}

GuestConfigParser::~GuestConfigParser() = default;

zx_status_t GuestConfigParser::ParseArgcArgv(int argc, char** argv) {
  fxl::CommandLine cl = fxl::CommandLineFromArgcArgv(argc, argv);

  if (cl.positional_args().size() > 0) {
    FXL_LOG(ERROR) << "Unknown positional option: " << cl.positional_args()[0];
    print_usage(cl);
    return ZX_ERR_INVALID_ARGS;
  }

  for (const fxl::CommandLine::Option& option : cl.options()) {
    auto entry = options_.find(option.name);
    if (entry == options_.end()) {
      FXL_LOG(ERROR) << "Unknown option --" << option.name;
      print_usage(cl);
      return ZX_ERR_INVALID_ARGS;
    }
    zx_status_t status = entry->second(option.name, option.value);
    if (status != ZX_OK) {
      print_usage(cl);
      return ZX_ERR_INVALID_ARGS;
    }
  }

  return ZX_OK;
}

zx_status_t GuestConfigParser::ParseConfig(const std::string& data) {
  rapidjson::Document document;
  document.Parse(data);
  if (!document.IsObject()) {
    return ZX_ERR_INVALID_ARGS;
  }

  for (auto& member : document.GetObject()) {
    auto entry = options_.find(member.name.GetString());
    if (entry == options_.end()) {
      FXL_LOG(ERROR) << "Unknown field in configuration object: "
                     << member.name.GetString();
      return ZX_ERR_INVALID_ARGS;
    }

    // For string members, invoke the handler directly on the value.
    if (member.value.IsString()) {
      zx_status_t status =
          entry->second(member.name.GetString(), member.value.GetString());
      if (status != ZX_OK) {
        return ZX_ERR_INVALID_ARGS;
      }
      continue;
    }

    // For array members, invoke the handler on each value in the array.
    if (member.value.IsArray()) {
      for (auto& array_member : member.value.GetArray()) {
        if (!array_member.IsString()) {
          FXL_LOG(ERROR) << "Array entry has incorect type, expected string: "
                         << member.name.GetString();
          return ZX_ERR_INVALID_ARGS;
        }
        zx_status_t status =
            entry->second(member.name.GetString(), array_member.GetString());
        if (status != ZX_OK) {
          return ZX_ERR_INVALID_ARGS;
        }
      }
      continue;
    }
    FXL_LOG(ERROR) << "Field has incorrect type, expected string or array: "
                   << member.name.GetString();
    return ZX_ERR_INVALID_ARGS;
  }

  return ZX_OK;
}
