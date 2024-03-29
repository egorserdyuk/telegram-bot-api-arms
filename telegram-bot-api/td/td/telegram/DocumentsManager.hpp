//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2023
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#pragma once

#include "td/telegram/DocumentsManager.h"

#include "td/telegram/files/FileId.hpp"
#include "td/telegram/PhotoSize.hpp"
#include "td/telegram/Version.h"

#include "td/utils/tl_helpers.h"

namespace td {

template <class StorerT>
void DocumentsManager::store_document(FileId file_id, StorerT &storer) const {
  const GeneralDocument *document = get_document(file_id);
  CHECK(document != nullptr);
  store(document->file_name, storer);
  store(document->mime_type, storer);
  store(document->minithumbnail, storer);
  store(document->thumbnail, storer);
  store(file_id, storer);
}

template <class ParserT>
FileId DocumentsManager::parse_document(ParserT &parser) {
  auto document = make_unique<GeneralDocument>();
  parse(document->file_name, parser);
  parse(document->mime_type, parser);
  if (parser.version() >= static_cast<int32>(Version::SupportMinithumbnails)) {
    parse(document->minithumbnail, parser);
  }
  parse(document->thumbnail, parser);
  parse(document->file_id, parser);
  if (parser.get_error() != nullptr || !document->file_id.is_valid()) {
    return FileId();
  }
  return on_get_document(std::move(document), false);
}

}  // namespace td
