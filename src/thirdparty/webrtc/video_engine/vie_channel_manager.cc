/*
 *  Copyright (c) 2012 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "video_engine/vie_channel_manager.h"

#include "engine_configurations.h"
#include "modules/rtp_rtcp/interface/rtp_rtcp.h"
#include "modules/utility/interface/process_thread.h"
#include "system_wrappers/interface/critical_section_wrapper.h"
#include "system_wrappers/interface/map_wrapper.h"
#include "system_wrappers/interface/trace.h"
#include "video_engine/vie_channel.h"
#include "video_engine/vie_defines.h"
#include "video_engine/vie_encoder.h"
#include "video_engine/vie_remb.h"
#include "voice_engine/main/interface/voe_video_sync.h"

namespace webrtc {

ViEChannelManager::ViEChannelManager(
    int engine_id,
    int number_of_cores,
    ViEPerformanceMonitor& vie_performance_monitor)
    : channel_id_critsect_(CriticalSectionWrapper::CreateCriticalSection()),
      engine_id_(engine_id),
      number_of_cores_(number_of_cores),
      vie_performance_monitor_(vie_performance_monitor),
      free_channel_ids_(new bool[kViEMaxNumberOfChannels]),
      free_channel_ids_size_(kViEMaxNumberOfChannels),
      voice_sync_interface_(NULL),
      remb_(new VieRemb(engine_id)),
      voice_engine_(NULL),
      module_process_thread_(NULL) {
  WEBRTC_TRACE(kTraceMemory, kTraceVideo, ViEId(engine_id),
               "ViEChannelManager::ViEChannelManager(engine_id: %d)",
               engine_id);
  for (int idx = 0; idx < free_channel_ids_size_; idx++) {
    free_channel_ids_[idx] = true;
  }
}

ViEChannelManager::~ViEChannelManager() {
  WEBRTC_TRACE(kTraceMemory, kTraceVideo, ViEId(engine_id_),
               "ViEChannelManager Destructor, engine_id: %d", engine_id_);

  module_process_thread_->DeRegisterModule(remb_.get());
  while (channel_map_.size() > 0) {
    ChannelMap::iterator it = channel_map_.begin();
    // DeleteChannel will erase this channel from the map and invalidate |it|.
    DeleteChannel(it->first);
  }

  if (voice_sync_interface_) {
    voice_sync_interface_->Release();
  }
  if (channel_id_critsect_) {
    delete channel_id_critsect_;
    channel_id_critsect_ = NULL;
  }
  if (free_channel_ids_) {
    delete[] free_channel_ids_;
    free_channel_ids_ = NULL;
    free_channel_ids_size_ = 0;
  }
}

void ViEChannelManager::SetModuleProcessThread(
    ProcessThread& module_process_thread) {
  assert(!module_process_thread_);
  module_process_thread_ = &module_process_thread;
  module_process_thread_->RegisterModule(remb_.get());
}

int ViEChannelManager::CreateChannel(int& channel_id) {
  CriticalSectionScoped cs(*channel_id_critsect_);

  // Get a free id for the new channel.
  if (!GetFreeChannelId(channel_id)) {
    WEBRTC_TRACE(kTraceError, kTraceVideo, ViEId(engine_id_),
                 "Max number of channels reached: %d", channel_map_.size());
    return -1;
  }

  ViEChannel* vie_channel = new ViEChannel(channel_id, engine_id_,
                                           number_of_cores_,
                                           *module_process_thread_);
  if (!vie_channel) {
    ReturnChannelId(channel_id);
    return -1;
  }
  if (vie_channel->Init() != 0) {
    WEBRTC_TRACE(kTraceError, kTraceVideo, ViEId(engine_id_),
                 "%s could not init channel", __FUNCTION__, channel_id);
    ReturnChannelId(channel_id);
    delete vie_channel;
    vie_channel = NULL;
    return -1;
  }

  // There is no ViEEncoder for this channel, create one with default settings.
  ViEEncoder* vie_encoder = new ViEEncoder(engine_id_, channel_id,
                                           number_of_cores_,
                                           *module_process_thread_);
  if (!vie_encoder) {
    WEBRTC_TRACE(kTraceError, kTraceVideo, ViEId(engine_id_),
                 "%s(video_channel_id: %d) - Could not create a new encoder",
                 __FUNCTION__, channel_id);
    delete vie_channel;
    return -1;
  }

  vie_encoder_map_[channel_id] = vie_encoder;
  channel_map_[channel_id] = vie_channel;

  // Register the channel at the encoder.
  RtpRtcp* send_rtp_rtcp_module = vie_encoder->SendRtpRtcpModule();
  if (vie_channel->RegisterSendRtpRtcpModule(*send_rtp_rtcp_module) != 0) {
    assert(false);
    vie_encoder_map_.erase(channel_id);
    channel_map_.erase(channel_id);
    ReturnChannelId(channel_id);
    delete vie_channel;
    WEBRTC_TRACE(kTraceError, kTraceVideo, ViEId(engine_id_, channel_id),
                 "%s: Could not register rtp module %d", __FUNCTION__,
                 channel_id);
    return -1;
  }
  return 0;
}

int ViEChannelManager::CreateChannel(int& channel_id, int original_channel) {
  CriticalSectionScoped cs(*channel_id_critsect_);

  // Check that original_channel already exists.
  ViEEncoder* vie_encoder = ViEEncoderPtr(original_channel);
  if (!vie_encoder) {
    WEBRTC_TRACE(kTraceError, kTraceVideo, ViEId(engine_id_),
                 "%s: Original channel doesn't exist", __FUNCTION__,
                 original_channel);
    return -1;
  }
  VideoCodec video_codec;
  if (vie_encoder->GetEncoder(video_codec) == 0) {
    if (video_codec.numberOfSimulcastStreams > 0) {
      WEBRTC_TRACE(kTraceError, kTraceVideo,
                   ViEId(engine_id_, original_channel),
                   "%s: Can't share a simulcast encoder",
                   __FUNCTION__);
      return -1;
    }
  }

  // Get a free id for the new channel.
  if (GetFreeChannelId(channel_id) == false) {
    WEBRTC_TRACE(kTraceError, kTraceVideo, ViEId(engine_id_),
                 "Max number of channels reached: %d", channel_map_.size());
    return -1;
  }
  ViEChannel* vie_channel = new ViEChannel(channel_id, engine_id_,
                                           number_of_cores_,
                                           *module_process_thread_);
  if (!vie_channel) {
    ReturnChannelId(channel_id);
    return -1;
  }
  if (vie_channel->Init() != 0) {
    WEBRTC_TRACE(kTraceError, kTraceVideo, ViEId(engine_id_),
                 "%s could not init channel", __FUNCTION__, channel_id);
    ReturnChannelId(channel_id);
    delete vie_channel;
    vie_channel = NULL;
    return -1;
  }
  vie_encoder_map_[channel_id] = vie_encoder;

  // Set the same encoder settings for the channel as used by the master
  // channel. Do this before attaching rtp module to ensure all rtp children has
  // the same codec type.
  VideoCodec encoder;
  if (vie_encoder->GetEncoder(encoder) == 0) {
    vie_channel->SetSendCodec(encoder);
  }
  channel_map_[channel_id] = vie_channel;

  // Register the channel at the encoder.
  RtpRtcp* send_rtp_rtcp_module = vie_encoder->SendRtpRtcpModule();
  if (vie_channel->RegisterSendRtpRtcpModule(*send_rtp_rtcp_module) != 0) {
    assert(false);
    vie_encoder_map_.erase(channel_id);
    channel_map_.erase(channel_id);
    ReturnChannelId(channel_id);
    delete vie_channel;
    WEBRTC_TRACE(kTraceError, kTraceVideo, ViEId(engine_id_, channel_id),
                 "%s: Could not register rtp module %d", __FUNCTION__,
                 channel_id);
    return -1;
  }
  return 0;
}

int ViEChannelManager::DeleteChannel(int channel_id) {
  ViEChannel* vie_channel = NULL;
  ViEEncoder* vie_encoder = NULL;
  {
    // Write lock to make sure no one is using the channel.
    ViEManagerWriteScoped wl(*this);

    // Protect the map.
    CriticalSectionScoped cs(*channel_id_critsect_);
    ChannelMap::iterator c_it = channel_map_.find(channel_id);
    if (c_it == channel_map_.end()) {
      // No such channel.
      WEBRTC_TRACE(kTraceError, kTraceVideo, ViEId(engine_id_),
                   "%s Channel doesn't exist: %d", __FUNCTION__, channel_id);
      return -1;
    }
    vie_channel = c_it->second;
    channel_map_.erase(c_it);

    // Deregister possible remb modules.
    RtpRtcp* rtp_module = vie_channel->rtp_rtcp();
    remb_->RemoveSendChannel(rtp_module);
    remb_->RemoveReceiveChannel(rtp_module);

    // Deregister the channel from the ViEEncoder to stop the media flow.
    vie_channel->DeregisterSendRtpRtcpModule();
    ReturnChannelId(channel_id);

    // Find the encoder object.
    EncoderMap::iterator e_it = vie_encoder_map_.find(channel_id);
    assert(e_it != vie_encoder_map_.end());
    vie_encoder = e_it->second;

    // Check if other channels are using the same encoder.
    if (ChannelUsingViEEncoder(channel_id)) {
      // Don't delete the ViEEncoder, at least one other channel is using it.
      WEBRTC_TRACE(kTraceInfo, kTraceVideo, ViEId(engine_id_),
        "%s ViEEncoder removed from map for channel %d, not deleted",
        __FUNCTION__, channel_id);
      vie_encoder = NULL;
    } else {
      WEBRTC_TRACE(kTraceInfo, kTraceVideo, ViEId(engine_id_),
                   "%s ViEEncoder deleted for channel %d", __FUNCTION__,
                   channel_id);
      // Delete later when we've released the critsect.
    }

    // We can't erase the item before we've checked for other channels using
    // same ViEEncoder.
    vie_encoder_map_.erase(e_it);
  }

  // Leave the write critsect before deleting the objects.
  // Deleting a channel can cause other objects, such as renderers, to be
  // deleted, which might take time.
  if (vie_encoder) {
    delete vie_encoder;
  }
  delete vie_channel;

  WEBRTC_TRACE(kTraceInfo, kTraceVideo, ViEId(engine_id_),
               "%s Channel %d deleted", __FUNCTION__, channel_id);
  return 0;
}

int ViEChannelManager::SetVoiceEngine(VoiceEngine* voice_engine) {
  // Write lock to make sure no one is using the channel.
  ViEManagerWriteScoped wl(*this);

  CriticalSectionScoped cs(*channel_id_critsect_);

  VoEVideoSync* sync_interface = NULL;
  if (voice_engine) {
    // Get new sync interface.
    sync_interface = VoEVideoSync::GetInterface(voice_engine);
    if (!sync_interface) {
      WEBRTC_TRACE(kTraceError, kTraceVideo, ViEId(engine_id_),
                   "%s Can't get audio sync interface from VoiceEngine.",
                   __FUNCTION__);
      return -1;
    }
  }

  for (ChannelMap::iterator it = channel_map_.begin(); it != channel_map_.end();
       ++it) {
    it->second->SetVoiceChannel(-1, sync_interface);
  }
  if (voice_sync_interface_) {
    voice_sync_interface_->Release();
  }
  voice_engine_ = voice_engine;
  voice_sync_interface_ = sync_interface;
  return 0;
}

int ViEChannelManager::ConnectVoiceChannel(int channel_id,
                                           int audio_channel_id) {
  CriticalSectionScoped cs(*channel_id_critsect_);
  if (!voice_sync_interface_) {
    WEBRTC_TRACE(kTraceError, kTraceVideo, ViEId(engine_id_, channel_id),
                 "No VoE set");
    return -1;
  }
  ViEChannel* channel = ViEChannelPtr(channel_id);
  if (!channel) {
    return -1;
  }
  return channel->SetVoiceChannel(audio_channel_id, voice_sync_interface_);
}

int ViEChannelManager::DisconnectVoiceChannel(int channel_id) {
  CriticalSectionScoped cs(*channel_id_critsect_);
  ViEChannel* channel = ViEChannelPtr(channel_id);
  if (channel) {
    channel->SetVoiceChannel(-1, NULL);
    return 0;
  }
  return -1;
}

VoiceEngine* ViEChannelManager::GetVoiceEngine() {
  CriticalSectionScoped cs(*channel_id_critsect_);
  return voice_engine_;
}

bool ViEChannelManager::SetRembStatus(int channel_id, bool sender,
                                      bool receiver) {
  CriticalSectionScoped cs(*channel_id_critsect_);
  ViEChannel* channel = ViEChannelPtr(channel_id);
  if (!channel) {
    return false;
  }

  if (sender || receiver) {
    if (!channel->EnableRemb(true)) {
      return false;
    }
  } else {
    channel->EnableRemb(false);
  }
  RtpRtcp* rtp_module = channel->rtp_rtcp();
  if (sender) {
    remb_->AddSendChannel(rtp_module);
  } else {
    remb_->RemoveSendChannel(rtp_module);
  }
  if (receiver) {
    remb_->AddReceiveChannel(rtp_module);
    rtp_module->SetRemoteBitrateObserver(remb_.get());
  } else {
    remb_->RemoveReceiveChannel(rtp_module);
    rtp_module->SetRemoteBitrateObserver(NULL);
  }
  return true;
}

ViEChannel* ViEChannelManager::ViEChannelPtr(int channel_id) const {
  CriticalSectionScoped cs(*channel_id_critsect_);
  ChannelMap::const_iterator it = channel_map_.find(channel_id);
  if (it == channel_map_.end()) {
    WEBRTC_TRACE(kTraceError, kTraceVideo, ViEId(engine_id_),
                 "%s Channel doesn't exist: %d", __FUNCTION__, channel_id);
    return NULL;
  }
  return it->second;
}

void ViEChannelManager::GetViEChannels(MapWrapper& channel_map) {
  CriticalSectionScoped cs(*channel_id_critsect_);
  if (channel_map.Size() == 0) {
    return;
  }

  // Add all items to 'channelMap'.
  for (ChannelMap::iterator it = channel_map_.begin(); it != channel_map_.end();
       ++it) {
    channel_map.Insert(it->first, it->second);
  }
}

ViEEncoder* ViEChannelManager::ViEEncoderPtr(int video_channel_id) const {
  CriticalSectionScoped cs(*channel_id_critsect_);
  EncoderMap::const_iterator it = vie_encoder_map_.find(video_channel_id);
  if (it == vie_encoder_map_.end()) {
    return NULL;
  }
  return it->second;
}

bool ViEChannelManager::GetFreeChannelId(int& free_channel_id) {
  CriticalSectionScoped cs(*channel_id_critsect_);
  int idx = 0;
  while (idx < free_channel_ids_size_) {
    if (free_channel_ids_[idx] == true) {
      // We've found a free id, allocate it and return.
      free_channel_ids_[idx] = false;
      free_channel_id = idx + kViEChannelIdBase;
      return true;
    }
    idx++;
  }
  // No free channel id.
  free_channel_id = -1;
  return false;
}

void ViEChannelManager::ReturnChannelId(int channel_id) {
  CriticalSectionScoped cs(*channel_id_critsect_);
  assert(channel_id < kViEMaxNumberOfChannels + kViEChannelIdBase &&
         channel_id >= kViEChannelIdBase);
  free_channel_ids_[channel_id - kViEChannelIdBase] = true;
}

bool ViEChannelManager::ChannelUsingViEEncoder(int channel_id) const {
  CriticalSectionScoped cs(*channel_id_critsect_);
  EncoderMap::const_iterator orig_it = vie_encoder_map_.find(channel_id);
  if(orig_it == vie_encoder_map_.end()) {
    // No ViEEncoder for this channel.
    return false;
  }

  // Loop through all other channels to see if anyone points at the same
  // ViEEncoder.
  for (EncoderMap::const_iterator comp_it = vie_encoder_map_.begin();
       comp_it != vie_encoder_map_.end(); ++comp_it) {
    // Make sure we're not comparing the same channel with itself.
    if (comp_it->first != channel_id) {
      if (comp_it->second == orig_it->second) {
        return true;
      }
    }
  }
  return false;
}

void ViEChannelManager::ChannelsUsingViEEncoder(int channel_id,
                                                ChannelList* channels) const {
  CriticalSectionScoped cs(*channel_id_critsect_);
  EncoderMap::const_iterator orig_it = vie_encoder_map_.find(channel_id);

  for (ChannelMap::const_iterator c_it = channel_map_.begin();
       c_it != channel_map_.end(); ++c_it) {
    EncoderMap::const_iterator comp_it = vie_encoder_map_.find(c_it->first);
    assert(comp_it != vie_encoder_map_.end());
    if (comp_it->second == orig_it->second) {
      channels->push_back(c_it->second);
    }
  }
}

ViEChannelManagerScoped::ViEChannelManagerScoped(
    const ViEChannelManager& vie_channel_manager)
    : ViEManagerScopedBase(vie_channel_manager) {
}

ViEChannel* ViEChannelManagerScoped::Channel(int vie_channel_id) const {
  return static_cast<const ViEChannelManager*>(vie_manager_)->ViEChannelPtr(
      vie_channel_id);
}
ViEEncoder* ViEChannelManagerScoped::Encoder(int vie_channel_id) const {
  return static_cast<const ViEChannelManager*>(vie_manager_)->ViEEncoderPtr(
      vie_channel_id);
}

bool ViEChannelManagerScoped::ChannelUsingViEEncoder(int channel_id) const {
  return (static_cast<const ViEChannelManager*>(vie_manager_))->
      ChannelUsingViEEncoder(channel_id);
}

void ViEChannelManagerScoped::ChannelsUsingViEEncoder(
    int channel_id, ChannelList* channels) const {
  (static_cast<const ViEChannelManager*>(vie_manager_))->
      ChannelsUsingViEEncoder(channel_id, channels);
}

}  // namespace webrtc
