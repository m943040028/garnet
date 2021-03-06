// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "garnet/bin/appmgr/root_application_loader.h"

#include <fcntl.h>

#include <utility>

#include "garnet/bin/appmgr/url_resolver.h"
#include "lib/fsl/io/fd.h"
#include "lib/fsl/vmo/file.h"
#include "lib/fxl/files/unique_fd.h"
#include "lib/fxl/logging.h"
#include "lib/fxl/strings/concatenate.h"

namespace app {

RootApplicationLoader::RootApplicationLoader(std::vector<std::string> path)
    : path_(std::move(path)) {}

RootApplicationLoader::~RootApplicationLoader() {}

void RootApplicationLoader::LoadApplication(
    const fidl::String& url,
    const ApplicationLoader::LoadApplicationCallback& callback) {
  std::string path = GetPathFromURL(url);
  if (path.empty()) {
    // TODO(abarth): Support URL schemes other than file:// by querying the host
    // for an application runner.
    FXL_LOG(ERROR) << "Cannot load " << url
                   << " because the scheme is not supported.";
  } else {
    fxl::UniqueFD fd(open(path.c_str(), O_RDONLY));
    if (!fd.is_valid() && path[0] != '/') {
      if (path.find('/') == std::string::npos) {
        // TODO(abarth): We're currently hardcoding version 0 of the package,
        // but we'll eventually need to do something smarter.
        std::string pkg_path = fxl::Concatenate({"/pkgfs/packages/", path, "/0"});
        fd.reset(open(pkg_path.c_str(), O_DIRECTORY | O_RDONLY));
        if (fd.is_valid()) {
          zx::channel directory = fsl::CloneChannelFromFileDescriptor(fd.get());
          if (directory) {
            ApplicationPackagePtr package = ApplicationPackage::New();
            package->directory = std::move(directory);
            package->resolved_url = fxl::Concatenate({"file://", pkg_path});
            callback(std::move(package));
            return;
          }
        }
      }
      for (const auto& entry : path_) {
        std::string qualified_path = fxl::Concatenate({entry, "/", path});
        fd.reset(open(qualified_path.c_str(), O_RDONLY));
        if (fd.is_valid()) {
          path = qualified_path;
          break;
        }
      }
    }
    fsl::SizedVmo data;
    if (fd.is_valid() && fsl::VmoFromFd(std::move(fd), &data)) {
      ApplicationPackagePtr package = ApplicationPackage::New();
      package->data = std::move(data).ToTransport();
      package->resolved_url = fxl::Concatenate({"file://", path});
      callback(std::move(package));
      return;
    }
    FXL_LOG(ERROR) << "Could not load url: " << url;
  }

  callback(nullptr);
}

}  // namespace app
