// Copyright 2016 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#pragma once

#include <vector>

#include "garnet/bin/flog_viewer/accumulator.h"
#include "garnet/bin/flog_viewer/channel_handler.h"
#include "lib/media/fidl/logs/media_type_converter_channel.fidl.h"

namespace flog {
namespace handlers {

class MediaTypeConverterAccumulator;

// Handler for MediaTypeConverterChannel messages.
class MediaTypeConverter : public ChannelHandler,
                           public media::logs::MediaTypeConverterChannel {
 public:
  MediaTypeConverter(const std::string& format);

  ~MediaTypeConverter() override;

  std::shared_ptr<Accumulator> GetAccumulator() override;

 protected:
  // ChannelHandler overrides.
  void HandleMessage(fidl::Message* message) override;

 private:
  // MediaTypeConverterChannel implementation.
  void BoundAs(uint64_t koid, const fidl::String& converter_type) override;

  void Config(media::MediaTypePtr input_type,
              media::MediaTypePtr output_type,
              uint64_t consumer_address,
              uint64_t producer_address) override;

 private:
  media::logs::MediaTypeConverterChannelStub stub_;
  std::shared_ptr<MediaTypeConverterAccumulator> accumulator_;
};

// Status of a media type converter as understood by MediaTypeConverter.
class MediaTypeConverterAccumulator : public Accumulator {
 public:
  MediaTypeConverterAccumulator();
  ~MediaTypeConverterAccumulator() override;

  // Accumulator overrides.
  void Print(std::ostream& os) override;

 private:
  std::string converter_type_;
  media::MediaTypePtr input_type_;
  media::MediaTypePtr output_type_;
  std::shared_ptr<Channel> consumer_channel_;
  std::shared_ptr<Channel> producer_channel_;

  friend class MediaTypeConverter;
};

}  // namespace handlers
}  // namespace flog
