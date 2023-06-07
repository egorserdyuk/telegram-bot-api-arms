//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2023
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#pragma once

#include "td/telegram/DialogId.h"
#include "td/telegram/Dimensions.h"
#include "td/telegram/files/FileId.h"
#include "td/telegram/MessageEntity.h"
#include "td/telegram/Photo.h"
#include "td/telegram/td_api.h"
#include "td/telegram/telegram_api.h"

#include "td/utils/common.h"
#include "td/utils/Status.h"

namespace td {

class Td;

class MessageExtendedMedia {
  enum class Type : int32 { Empty, Unsupported, Preview, Photo, Video };
  Type type_ = Type::Empty;
  FormattedText caption_;

  static constexpr int32 CURRENT_VERSION = 1;

  // for Unsupported
  int32 unsupported_version_ = 0;

  // for Preview
  int32 duration_ = 0;
  Dimensions dimensions_;
  string minithumbnail_;

  // for Photo
  Photo photo_;

  // for Video
  FileId video_file_id_;

  friend bool operator==(const MessageExtendedMedia &lhs, const MessageExtendedMedia &rhs);

  bool is_media() const {
    return type_ != Type::Empty && type_ != Type::Preview;
  }

 public:
  MessageExtendedMedia() = default;

  MessageExtendedMedia(Td *td, telegram_api::object_ptr<telegram_api::MessageExtendedMedia> &&extended_media,
                       FormattedText &&caption, DialogId owner_dialog_id);

  static Result<MessageExtendedMedia> get_message_extended_media(
      Td *td, td_api::object_ptr<td_api::InputMessageContent> &&extended_media_content, DialogId owner_dialog_id,
      bool is_premium);

  bool is_empty() const {
    return type_ == Type::Empty;
  }

  void update_from(const MessageExtendedMedia &old_extended_media);

  bool update_to(Td *td, telegram_api::object_ptr<telegram_api::MessageExtendedMedia> extended_media_ptr,
                 DialogId owner_dialog_id);

  td_api::object_ptr<td_api::MessageExtendedMedia> get_message_extended_media_object(Td *td, bool skip_bot_commands,
                                                                                     int32 max_media_timestamp) const;

  void append_file_ids(const Td *td, vector<FileId> &file_ids) const;

  void delete_thumbnail(Td *td);

  bool need_reget() const {
    return type_ == Type::Unsupported && unsupported_version_ < CURRENT_VERSION;
  }

  bool need_poll() const {
    return type_ == Type::Preview;
  }

  bool has_media_timestamp() const {
    return type_ == Type::Video;
  }

  bool is_equal_but_different(const MessageExtendedMedia &other) const;

  int32 get_duration(const Td *td) const;

  const FormattedText *get_caption() const {
    return &caption_;
  }

  FileId get_upload_file_id() const;

  FileId get_any_file_id() const;

  FileId get_thumbnail_file_id(const Td *td) const;

  telegram_api::object_ptr<telegram_api::InputMedia> get_input_media(
      Td *td, tl_object_ptr<telegram_api::InputFile> input_file,
      tl_object_ptr<telegram_api::InputFile> input_thumbnail) const;

  template <class StorerT>
  void store(StorerT &storer) const;

  template <class ParserT>
  void parse(ParserT &parser);
};

bool operator==(const MessageExtendedMedia &lhs, const MessageExtendedMedia &rhs);

bool operator!=(const MessageExtendedMedia &lhs, const MessageExtendedMedia &rhs);

}  // namespace td
