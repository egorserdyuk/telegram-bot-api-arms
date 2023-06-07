//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2023
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#pragma once

#include "td/telegram/CustomEmojiId.h"
#include "td/telegram/td_api.h"
#include "td/telegram/telegram_api.h"

#include "td/utils/common.h"

namespace td {

class EmojiGroup {
  string title_;
  CustomEmojiId icon_custom_emoji_id_;
  vector<string> emojis_;

 public:
  EmojiGroup() = default;

  explicit EmojiGroup(telegram_api::object_ptr<telegram_api::emojiGroup> &&emoji_group);

  td_api::object_ptr<td_api::emojiCategory> get_emoji_category_object() const;

  template <class StorerT>
  void store(StorerT &storer) const;

  template <class ParserT>
  void parse(ParserT &parser);
};

class EmojiGroupList {
  string used_language_codes_;
  int32 hash_ = 0;
  vector<EmojiGroup> emoji_groups_;
  double next_reload_time_ = 0.0;

 public:
  EmojiGroupList() = default;

  EmojiGroupList(string used_language_codes, int32 hash,
                 vector<telegram_api::object_ptr<telegram_api::emojiGroup>> &&emoji_groups);

  td_api::object_ptr<td_api::emojiCategories> get_emoji_categories_object() const;

  const string &get_used_language_codes() const {
    return used_language_codes_;
  }

  int32 get_hash() const {
    return hash_;
  }

  bool is_expired() const;

  void update_next_reload_time();

  template <class StorerT>
  void store(StorerT &storer) const;

  template <class ParserT>
  void parse(ParserT &parser);
};

}  // namespace td
