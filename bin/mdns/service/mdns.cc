// Copyright 2017 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <iostream>
#include <limits>
#include <unordered_set>

#include "garnet/bin/mdns/service/mdns.h"

#include "garnet/bin/mdns/service/address_prober.h"
#include "garnet/bin/mdns/service/address_responder.h"
#include "garnet/bin/mdns/service/dns_formatting.h"
#include "garnet/bin/mdns/service/host_name_resolver.h"
#include "garnet/bin/mdns/service/instance_prober.h"
#include "garnet/bin/mdns/service/instance_responder.h"
#include "garnet/bin/mdns/service/instance_subscriber.h"
#include "garnet/bin/mdns/service/mdns_addresses.h"
#include "garnet/bin/mdns/service/mdns_names.h"
#include "garnet/bin/mdns/service/resource_renewer.h"
#include "lib/fsl/tasks/message_loop.h"
#include "lib/fxl/logging.h"
#include "lib/fxl/time/time_delta.h"

namespace mdns {

Mdns::Mdns() : task_runner_(fsl::MessageLoop::GetCurrent()->task_runner()) {}

Mdns::~Mdns() {}

void Mdns::EnableInterface(const std::string& name, sa_family_t family) {
  transceiver_.EnableInterface(name, family);
}

void Mdns::SetVerbose(bool verbose) {
  verbose_ = verbose;
}

void Mdns::Start(std::unique_ptr<InterfaceMonitor> interface_monitor,
                 const std::string& host_name) {
  FXL_DCHECK(!host_name.empty());
  FXL_DCHECK(state_ == State::kNotStarted);

  state_ = State::kWaitingForInterfaces;

  original_host_name_ = host_name;

  // Create a resource renewer agent to keep resources alive.
  resource_renewer_ = std::make_shared<ResourceRenewer>(this);

  // Create an address responder agent to respond to address queries.
  AddAgent(std::make_shared<AddressResponder>(this));

  transceiver_.Start(
      std::move(interface_monitor),
      [this]() {
        // TODO(dalesat): Link changes that create host name conflicts.
        // Once we have a NIC and we've decided on a unique host name, we
        // don't do any more address probes. This means that we could have link
        // changes that cause two hosts with the same name to be on the same
        // subnet. To improve matters, we need to be prepared to change a host
        // name we've been using for awhile.
        // TODO(dalesat): Add option to skip address probe.
        // The mDNS spec is explicit about the need for address probes and
        // that host names should be user-friendly. Many embedded devices, on
        // the other hand, use host names that are guaranteed unique by virtue
        // of including large random values, serial numbers, etc. This mDNS
        // implementation should offer the option of turning off address probes
        // for such devices.
        if (state_ == State::kWaitingForInterfaces &&
            transceiver_.has_interfaces()) {
          StartAddressProbe(original_host_name_);
        }
      },
      [this](std::unique_ptr<DnsMessage> message,
             const ReplyAddress& reply_address) {
        if (verbose_) {
          FXL_LOG(INFO) << "Inbound message from " << reply_address << ":"
                        << *message;
        }

        for (auto& question : message->questions_) {
          // We reply to questions using unicast if specifically requested in
          // the question or if the sender's port isn't 5353.
          ReceiveQuestion(*question, (question->unicast_response_ ||
                                      reply_address.socket_address().port() !=
                                          MdnsAddresses::kMdnsPort)
                                         ? reply_address
                                         : MdnsAddresses::kV4MulticastReply);
        }

        for (auto& resource : message->answers_) {
          ReceiveResource(*resource, MdnsResourceSection::kAnswer);
        }

        for (auto& resource : message->authorities_) {
          ReceiveResource(*resource, MdnsResourceSection::kAuthority);
        }

        for (auto& resource : message->additionals_) {
          ReceiveResource(*resource, MdnsResourceSection::kAdditional);
        }

        resource_renewer_->EndOfMessage();
        DPROHIBIT_AGENT_REMOVAL();
        for (auto& pair : agents_) {
          pair.second->EndOfMessage();
        }
        DALLOW_AGENT_REMOVAL();

        SendMessages();
      });

  // The interface monitor may have already found interfaces. In that case,
  // start the address probe in case we don't get any link change notifications.
  if (state_ == State::kWaitingForInterfaces && transceiver_.has_interfaces()) {
    StartAddressProbe(original_host_name_);
  }
}

void Mdns::Stop() {
  transceiver_.Stop();
  state_ = State::kNotStarted;
}

void Mdns::ResolveHostName(const std::string& host_name,
                           fxl::TimePoint timeout,
                           const ResolveHostNameCallback& callback) {
  FXL_DCHECK(MdnsNames::IsValidHostName(host_name));
  FXL_DCHECK(callback);

  AddAgent(
      std::make_shared<HostNameResolver>(this, host_name, timeout, callback));
}

std::shared_ptr<MdnsAgent> Mdns::SubscribeToService(
    const std::string& service_name,
    const ServiceInstanceCallback& callback) {
  FXL_DCHECK(MdnsNames::IsValidServiceName(service_name));
  FXL_DCHECK(callback);

  auto agent =
      std::make_shared<InstanceSubscriber>(this, service_name, callback);

  AddAgent(agent);
  return agent;
}

bool Mdns::PublishServiceInstance(const std::string& service_name,
                                  const std::string& instance_name,
                                  IpPort port,
                                  const std::vector<std::string>& text,
                                  const PublishCallback& callback) {
  MdnsPublicationPtr publication = MdnsPublication::New();
  publication->port = port.as_uint16_t();
  publication->text = fidl::Array<fidl::String>::From(text);

  auto agent = std::make_shared<InstanceResponder>(
      this, service_name, instance_name, std::move(publication), callback);

  return ProbeAndAddInstanceResponder(service_name, instance_name, port, agent);
}

bool Mdns::UnpublishServiceInstance(const std::string& service_name,
                                    const std::string& instance_name) {
  FXL_DCHECK(MdnsNames::IsValidServiceName(service_name));
  FXL_DCHECK(MdnsNames::IsValidInstanceName(instance_name));

  std::string instance_full_name =
      MdnsNames::LocalInstanceFullName(instance_name, service_name);

  auto iter =
      instance_publishers_by_instance_full_name_.find(instance_full_name);

  if (iter == instance_publishers_by_instance_full_name_.end()) {
    return false;
  }

  iter->second->Quit();

  return true;
}

bool Mdns::AddResponder(const std::string& service_name,
                        const std::string& instance_name,
                        fidl::InterfaceHandle<MdnsResponder> responder) {
  auto agent = std::make_shared<InstanceResponder>(
      this, service_name, instance_name, std::move(responder));

  // We're using a bogus port number here, which is OK, because the 'proposed'
  // resource created from it is only used for collision resolution.
  return ProbeAndAddInstanceResponder(service_name, instance_name,
                                      IpPort::From_uint16_t(0), agent);
}

bool Mdns::SetSubtypes(const std::string& service_name,
                       const std::string& instance_name,
                       std::vector<std::string> subtypes) {
  FXL_DCHECK(MdnsNames::IsValidServiceName(service_name));
  FXL_DCHECK(MdnsNames::IsValidInstanceName(instance_name));

  std::string instance_full_name =
      MdnsNames::LocalInstanceFullName(instance_name, service_name);

  auto iter =
      instance_publishers_by_instance_full_name_.find(instance_full_name);

  if (iter == instance_publishers_by_instance_full_name_.end()) {
    return false;
  }

  iter->second->SetSubtypes(std::move(subtypes));
  SendMessages();

  return true;
}

bool Mdns::ReannounceInstance(const std::string& service_name,
                              const std::string& instance_name) {
  FXL_DCHECK(MdnsNames::IsValidServiceName(service_name));
  FXL_DCHECK(MdnsNames::IsValidInstanceName(instance_name));

  std::string instance_full_name =
      MdnsNames::LocalInstanceFullName(instance_name, service_name);

  auto iter =
      instance_publishers_by_instance_full_name_.find(instance_full_name);

  if (iter == instance_publishers_by_instance_full_name_.end()) {
    return false;
  }

  iter->second->Reannounce();
  SendMessages();

  return true;
}

void Mdns::StartAddressProbe(const std::string& host_name) {
  state_ = State::kAddressProbeInProgress;

  host_name_ = host_name;
  host_full_name_ = MdnsNames::LocalHostFullName(host_name);

  std::cerr << "mDNS: Verifying uniqueness of host name " << host_full_name_
            << "\n";

  transceiver_.SetHostFullName(host_full_name_);

  address_placeholder_ =
      std::make_shared<DnsResource>(host_full_name_, DnsType::kA);

  // Create an address prober to look for host name conflicts. The address
  // prober removes itself immediately before it calls the callback.
  auto address_prober =
      std::make_shared<AddressProber>(this, [this](bool successful) {
        FXL_DCHECK(agents_.empty());

        if (!successful) {
          std::cerr << "mDNS: Another host is using name " << host_full_name_
                    << "\n";
          OnHostNameConflict();
          return;
        }

        std::cerr << "mDNS: Using unique host name " << host_full_name_ << "\n";

        // Start all the agents.
        state_ = State::kActive;

        // |resource_renewer_| doesn't need to be started, but we do it
        // anyway in case that changes.
        resource_renewer_->Start(host_full_name_);

        for (auto agent : agents_awaiting_start_) {
          AddAgent(agent);
        }

        agents_awaiting_start_.clear();
      });

  // We don't use |AddAgent| here, because agents added that way don't actually
  // participate until we're done probing for host name conflicts.
  agents_.emplace(address_prober.get(), address_prober);
  address_prober->Start(host_full_name_);
  SendMessages();
}

void Mdns::OnHostNameConflict() {
  // TODO(dalesat): Support other renaming strategies?
  std::ostringstream os;
  os << original_host_name_ << next_host_name_deduplicator_;
  ++next_host_name_deduplicator_;

  StartAddressProbe(os.str());
}

void Mdns::PostTaskForTime(MdnsAgent* agent,
                           fxl::Closure task,
                           fxl::TimePoint target_time) {
  task_queue_.emplace(agent, task, target_time);
  PostTask();
}

void Mdns::SendQuestion(std::shared_ptr<DnsQuestion> question) {
  FXL_DCHECK(question);
  DnsMessage& message =
      outbound_messages_by_reply_address_[MdnsAddresses::kV4MulticastReply];
  message.questions_.push_back(question);
}

void Mdns::SendResource(std::shared_ptr<DnsResource> resource,
                        MdnsResourceSection section,
                        const ReplyAddress& reply_address) {
  FXL_DCHECK(resource);

  if (section == MdnsResourceSection::kExpired) {
    // Expirations are distributed to local agents. We handle this case
    // separately so we don't create an empty outbound message.
    prohibit_agent_removal_ = true;

    for (auto& pair : agents_) {
      pair.second->ReceiveResource(*resource, MdnsResourceSection::kExpired);
    }

    prohibit_agent_removal_ = false;
    return;
  }

  DnsMessage& message = outbound_messages_by_reply_address_[reply_address];

  switch (section) {
    case MdnsResourceSection::kAnswer:
      message.answers_.push_back(resource);
      break;
    case MdnsResourceSection::kAuthority:
      message.authorities_.push_back(resource);
      break;
    case MdnsResourceSection::kAdditional:
      message.additionals_.push_back(resource);
      break;
    case MdnsResourceSection::kExpired:
      FXL_DCHECK(false);
      break;
  }
}

void Mdns::SendAddresses(MdnsResourceSection section,
                         const ReplyAddress& reply_address) {
  SendResource(address_placeholder_, section, reply_address);
}

void Mdns::Renew(const DnsResource& resource) {
  resource_renewer_->Renew(resource);
}

void Mdns::RemoveAgent(const MdnsAgent* agent,
                       const std::string& published_instance_full_name) {
  FXL_DCHECK(agent);
  FXL_DCHECK(!prohibit_agent_removal_);

  agents_.erase(agent);

  // Remove all pending tasks posted by this agent.
  std::priority_queue<TaskQueueEntry> temp;
  task_queue_.swap(temp);

  while (!temp.empty()) {
    if (temp.top().agent_ != agent) {
      task_queue_.emplace(temp.top().agent_, temp.top().task_,
                          temp.top().time_);
    }

    temp.pop();
  }

  if (!published_instance_full_name.empty()) {
    instance_probers_by_instance_full_name_.erase(published_instance_full_name);
    instance_publishers_by_instance_full_name_.erase(
        published_instance_full_name);
  }

  // In case the agent sent an epitaph.
  SendMessages();
}

void Mdns::AddAgent(std::shared_ptr<MdnsAgent> agent) {
  if (state_ == State::kActive) {
    agents_.emplace(agent.get(), agent);
    FXL_DCHECK(!host_full_name_.empty());
    agent->Start(host_full_name_);
    SendMessages();
  } else {
    agents_awaiting_start_.push_back(agent);
  }
}

bool Mdns::ProbeAndAddInstanceResponder(
    const std::string& service_name,
    const std::string& instance_name,
    IpPort port,
    std::shared_ptr<InstanceResponder> agent) {
  FXL_DCHECK(MdnsNames::IsValidServiceName(service_name));
  FXL_DCHECK(MdnsNames::IsValidInstanceName(instance_name));

  std::string instance_full_name =
      MdnsNames::LocalInstanceFullName(instance_name, service_name);

  if (instance_probers_by_instance_full_name_.find(instance_full_name) !=
          instance_probers_by_instance_full_name_.end() ||
      instance_publishers_by_instance_full_name_.find(instance_full_name) !=
          instance_publishers_by_instance_full_name_.end()) {
    agent->UpdateStatus(MdnsResult::ALREADY_PUBLISHED_LOCALLY);
    return false;
  }

  auto prober = std::make_shared<InstanceProber>(
      this, service_name, instance_name, port,
      [this, instance_full_name, agent](bool successful) {
        if (!successful) {
          agent->UpdateStatus(MdnsResult::ALREADY_PUBLISHED_ON_SUBNET);
          return;
        }

        agent->UpdateStatus(MdnsResult::OK);

        AddAgent(agent);
        instance_publishers_by_instance_full_name_.emplace(instance_full_name,
                                                           agent);
      });

  AddAgent(prober);
  instance_probers_by_instance_full_name_.emplace(instance_full_name, prober);

  return true;
}

void Mdns::SendMessages() {
  for (auto& pair : outbound_messages_by_reply_address_) {
    const ReplyAddress& reply_address = pair.first;
    DnsMessage& message = pair.second;

    message.UpdateCounts();

    if (message.questions_.empty()) {
      message.header_.SetResponse(true);
      message.header_.SetAuthoritativeAnswer(true);
    }

    if (verbose_) {
      if (reply_address == MdnsAddresses::kV4MulticastReply) {
        FXL_LOG(INFO) << "Outbound message (multicast): " << message;
      } else {
        FXL_LOG(INFO) << "Outbound message to " << reply_address << ":"
                      << message;
      }
    }

    transceiver_.SendMessage(&message, reply_address);
  }

  outbound_messages_by_reply_address_.clear();
}

void Mdns::ReceiveQuestion(const DnsQuestion& question,
                           const ReplyAddress& reply_address) {
  // Renewer doesn't need questions.
  DPROHIBIT_AGENT_REMOVAL();
  for (auto& pair : agents_) {
    pair.second->ReceiveQuestion(question, reply_address);
  }
  DALLOW_AGENT_REMOVAL();
}

void Mdns::ReceiveResource(const DnsResource& resource,
                           MdnsResourceSection section) {
  // Renewer is always first.
  resource_renewer_->ReceiveResource(resource, section);
  DPROHIBIT_AGENT_REMOVAL();
  for (auto& pair : agents_) {
    pair.second->ReceiveResource(resource, section);
  }
  DALLOW_AGENT_REMOVAL();
}

void Mdns::PostTask() {
  FXL_DCHECK(!task_queue_.empty());

  if (task_queue_.top().time_ >= posted_task_time_) {
    return;
  }

  posted_task_time_ = task_queue_.top().time_;

  task_runner_->PostTaskForTime(
      [this]() {
        // Suppress recursive calls to this method.
        posted_task_time_ = fxl::TimePoint::Min();

        fxl::TimePoint now = fxl::TimePoint::Now();

        while (!task_queue_.empty() && task_queue_.top().time_ <= now) {
          fxl::Closure task = task_queue_.top().task_;
          task_queue_.pop();
          task();
        }

        SendMessages();

        posted_task_time_ = fxl::TimePoint::Max();
        if (!task_queue_.empty()) {
          PostTask();
        }
      },
      posted_task_time_);
}

}  // namespace mdns
