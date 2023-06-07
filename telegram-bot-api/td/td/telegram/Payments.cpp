//
// Copyright Aliaksei Levin (levlam@telegram.org), Arseny Smirnov (arseny30@gmail.com) 2014-2023
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)
//
#include "td/telegram/Payments.h"

#include "td/telegram/AccessRights.h"
#include "td/telegram/ContactsManager.h"
#include "td/telegram/DialogId.h"
#include "td/telegram/Global.h"
#include "td/telegram/InputInvoice.h"
#include "td/telegram/MessageEntity.h"
#include "td/telegram/MessageId.h"
#include "td/telegram/MessagesManager.h"
#include "td/telegram/misc.h"
#include "td/telegram/PasswordManager.h"
#include "td/telegram/Photo.h"
#include "td/telegram/ServerMessageId.h"
#include "td/telegram/Td.h"
#include "td/telegram/telegram_api.h"
#include "td/telegram/ThemeManager.h"
#include "td/telegram/UpdatesManager.h"
#include "td/telegram/UserId.h"

#include "td/utils/algorithm.h"
#include "td/utils/buffer.h"
#include "td/utils/common.h"
#include "td/utils/JsonBuilder.h"
#include "td/utils/logging.h"
#include "td/utils/Status.h"

namespace td {

namespace {

struct InputInvoiceInfo {
  DialogId dialog_id_;
  telegram_api::object_ptr<telegram_api::InputInvoice> input_invoice_;
};

Result<InputInvoiceInfo> get_input_invoice_info(Td *td, td_api::object_ptr<td_api::InputInvoice> &&input_invoice) {
  if (input_invoice == nullptr) {
    return Status::Error(400, "Input invoice must be non-empty");
  }

  InputInvoiceInfo result;
  switch (input_invoice->get_id()) {
    case td_api::inputInvoiceMessage::ID: {
      auto invoice = td_api::move_object_as<td_api::inputInvoiceMessage>(input_invoice);
      DialogId dialog_id(invoice->chat_id_);
      MessageId message_id(invoice->message_id_);
      TRY_RESULT(server_message_id, td->messages_manager_->get_invoice_message_id({dialog_id, message_id}));

      auto input_peer = td->messages_manager_->get_input_peer(dialog_id, AccessRights::Read);
      if (input_peer == nullptr) {
        return Status::Error(400, "Can't access the chat");
      }

      result.dialog_id_ = dialog_id;
      result.input_invoice_ =
          make_tl_object<telegram_api::inputInvoiceMessage>(std::move(input_peer), server_message_id.get());
      break;
    }
    case td_api::inputInvoiceName::ID: {
      auto invoice = td_api::move_object_as<td_api::inputInvoiceName>(input_invoice);
      result.input_invoice_ = make_tl_object<telegram_api::inputInvoiceSlug>(invoice->name_);
      break;
    }
    default:
      UNREACHABLE();
  }
  return std::move(result);
}

}  // namespace

class SetBotShippingAnswerQuery final : public Td::ResultHandler {
  Promise<Unit> promise_;

 public:
  explicit SetBotShippingAnswerQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(int64 shipping_query_id, const string &error_message,
            vector<tl_object_ptr<telegram_api::shippingOption>> &&shipping_options) {
    int32 flags = 0;
    if (!error_message.empty()) {
      flags |= telegram_api::messages_setBotShippingResults::ERROR_MASK;
    }
    if (!shipping_options.empty()) {
      flags |= telegram_api::messages_setBotShippingResults::SHIPPING_OPTIONS_MASK;
    }
    send_query(G()->net_query_creator().create(telegram_api::messages_setBotShippingResults(
        flags, shipping_query_id, error_message, std::move(shipping_options))));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::messages_setBotShippingResults>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    bool result = result_ptr.ok();
    if (!result) {
      LOG(INFO) << "Sending answer to a shipping query has failed";
    }
    promise_.set_value(Unit());
  }

  void on_error(Status status) final {
    promise_.set_error(std::move(status));
  }
};

class SetBotPreCheckoutAnswerQuery final : public Td::ResultHandler {
  Promise<Unit> promise_;

 public:
  explicit SetBotPreCheckoutAnswerQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(int64 pre_checkout_query_id, const string &error_message) {
    int32 flags = 0;
    if (!error_message.empty()) {
      flags |= telegram_api::messages_setBotPrecheckoutResults::ERROR_MASK;
    } else {
      flags |= telegram_api::messages_setBotPrecheckoutResults::SUCCESS_MASK;
    }

    send_query(G()->net_query_creator().create(telegram_api::messages_setBotPrecheckoutResults(
        flags, false /*ignored*/, pre_checkout_query_id, error_message)));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::messages_setBotPrecheckoutResults>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    bool result = result_ptr.ok();
    if (!result) {
      LOG(INFO) << "Sending answer to a pre-checkout query has failed";
    }
    promise_.set_value(Unit());
  }

  void on_error(Status status) final {
    promise_.set_error(std::move(status));
  }
};

static tl_object_ptr<td_api::labeledPricePart> convert_labeled_price(
    tl_object_ptr<telegram_api::labeledPrice> labeled_price) {
  CHECK(labeled_price != nullptr);
  if (!check_currency_amount(labeled_price->amount_)) {
    LOG(ERROR) << "Receive invalid labeled price amount " << labeled_price->amount_;
    labeled_price->amount_ = (labeled_price->amount_ < 0 ? -1 : 1) * (static_cast<int64>(1) << 40);
  }
  return make_tl_object<td_api::labeledPricePart>(std::move(labeled_price->label_), labeled_price->amount_);
}

static tl_object_ptr<td_api::invoice> convert_invoice(tl_object_ptr<telegram_api::invoice> invoice) {
  CHECK(invoice != nullptr);

  auto labeled_prices = transform(std::move(invoice->prices_), convert_labeled_price);
  bool is_test = (invoice->flags_ & telegram_api::invoice::TEST_MASK) != 0;
  bool need_name = (invoice->flags_ & telegram_api::invoice::NAME_REQUESTED_MASK) != 0;
  bool need_phone_number = (invoice->flags_ & telegram_api::invoice::PHONE_REQUESTED_MASK) != 0;
  bool need_email_address = (invoice->flags_ & telegram_api::invoice::EMAIL_REQUESTED_MASK) != 0;
  bool need_shipping_address = (invoice->flags_ & telegram_api::invoice::SHIPPING_ADDRESS_REQUESTED_MASK) != 0;
  bool send_phone_number_to_provider = (invoice->flags_ & telegram_api::invoice::PHONE_TO_PROVIDER_MASK) != 0;
  bool send_email_address_to_provider = (invoice->flags_ & telegram_api::invoice::EMAIL_TO_PROVIDER_MASK) != 0;
  bool is_flexible = (invoice->flags_ & telegram_api::invoice::FLEXIBLE_MASK) != 0;
  if (send_phone_number_to_provider) {
    need_phone_number = true;
  }
  if (send_email_address_to_provider) {
    need_email_address = true;
  }
  if (is_flexible) {
    need_shipping_address = true;
  }

  if (invoice->max_tip_amount_ < 0 || !check_currency_amount(invoice->max_tip_amount_)) {
    LOG(ERROR) << "Receive invalid maximum tip amount " << invoice->max_tip_amount_;
    invoice->max_tip_amount_ = 0;
  }
  td::remove_if(invoice->suggested_tip_amounts_,
                [](int64 amount) { return amount < 0 || !check_currency_amount(amount); });
  if (invoice->suggested_tip_amounts_.size() > 4) {
    invoice->suggested_tip_amounts_.resize(4);
  }

  return make_tl_object<td_api::invoice>(std::move(invoice->currency_), std::move(labeled_prices),
                                         invoice->max_tip_amount_, std::move(invoice->suggested_tip_amounts_),
                                         std::move(invoice->recurring_terms_url_), is_test, need_name,
                                         need_phone_number, need_email_address, need_shipping_address,
                                         send_phone_number_to_provider, send_email_address_to_provider, is_flexible);
}

static tl_object_ptr<td_api::PaymentProvider> convert_payment_provider(
    const string &native_provider_name, tl_object_ptr<telegram_api::dataJSON> native_parameters) {
  if (native_parameters == nullptr) {
    return nullptr;
  }

  if (native_provider_name == "smartglocal") {
    string data = native_parameters->data_;
    auto r_value = json_decode(data);
    if (r_value.is_error()) {
      LOG(ERROR) << "Can't parse JSON object \"" << native_parameters->data_ << "\": " << r_value.error();
      return nullptr;
    }

    auto value = r_value.move_as_ok();
    if (value.type() != JsonValue::Type::Object) {
      LOG(ERROR) << "Wrong JSON data \"" << native_parameters->data_ << '"';
      return nullptr;
    }

    auto r_public_token = get_json_object_string_field(value.get_object(), "public_token", false);

    if (r_public_token.is_error()) {
      LOG(ERROR) << "Unsupported JSON data \"" << native_parameters->data_ << '"';
      return nullptr;
    }
    if (value.get_object().size() != 1) {
      LOG(ERROR) << "Unsupported JSON data \"" << native_parameters->data_ << '"';
    }

    return make_tl_object<td_api::paymentProviderSmartGlocal>(r_public_token.move_as_ok());
  }
  if (native_provider_name == "stripe") {
    string data = native_parameters->data_;
    auto r_value = json_decode(data);
    if (r_value.is_error()) {
      LOG(ERROR) << "Can't parse JSON object \"" << native_parameters->data_ << "\": " << r_value.error();
      return nullptr;
    }

    auto value = r_value.move_as_ok();
    if (value.type() != JsonValue::Type::Object) {
      LOG(ERROR) << "Wrong JSON data \"" << native_parameters->data_ << '"';
      return nullptr;
    }

    auto r_need_country = get_json_object_bool_field(value.get_object(), "need_country", false);
    auto r_need_postal_code = get_json_object_bool_field(value.get_object(), "need_zip", false);
    auto r_need_cardholder_name = get_json_object_bool_field(value.get_object(), "need_cardholder_name", false);
    auto r_publishable_key = get_json_object_string_field(value.get_object(), "publishable_key", false);
    // TODO support "gpay_parameters":{"gateway":"stripe","stripe:publishableKey":"...","stripe:version":"..."}

    if (r_need_country.is_error() || r_need_postal_code.is_error() || r_need_cardholder_name.is_error() ||
        r_publishable_key.is_error()) {
      LOG(ERROR) << "Unsupported JSON data \"" << native_parameters->data_ << '"';
      return nullptr;
    }
    if (value.get_object().size() != 5) {
      LOG(ERROR) << "Unsupported JSON data \"" << native_parameters->data_ << '"';
    }

    return make_tl_object<td_api::paymentProviderStripe>(r_publishable_key.move_as_ok(), r_need_country.move_as_ok(),
                                                         r_need_postal_code.move_as_ok(),
                                                         r_need_cardholder_name.move_as_ok());
  }

  return nullptr;
}

static tl_object_ptr<td_api::address> convert_address(tl_object_ptr<telegram_api::postAddress> address) {
  if (address == nullptr) {
    return nullptr;
  }
  return make_tl_object<td_api::address>(std::move(address->country_iso2_), std::move(address->state_),
                                         std::move(address->city_), std::move(address->street_line1_),
                                         std::move(address->street_line2_), std::move(address->post_code_));
}

static tl_object_ptr<telegram_api::postAddress> convert_address(tl_object_ptr<td_api::address> address) {
  if (address == nullptr) {
    return nullptr;
  }
  return make_tl_object<telegram_api::postAddress>(std::move(address->street_line1_), std::move(address->street_line2_),
                                                   std::move(address->city_), std::move(address->state_),
                                                   std::move(address->country_code_), std::move(address->postal_code_));
}

static tl_object_ptr<td_api::orderInfo> convert_order_info(
    tl_object_ptr<telegram_api::paymentRequestedInfo> order_info) {
  if (order_info == nullptr) {
    return nullptr;
  }
  return make_tl_object<td_api::orderInfo>(std::move(order_info->name_), std::move(order_info->phone_),
                                           std::move(order_info->email_),
                                           convert_address(std::move(order_info->shipping_address_)));
}

static tl_object_ptr<td_api::shippingOption> convert_shipping_option(
    tl_object_ptr<telegram_api::shippingOption> shipping_option) {
  if (shipping_option == nullptr) {
    return nullptr;
  }

  return make_tl_object<td_api::shippingOption>(std::move(shipping_option->id_), std::move(shipping_option->title_),
                                                transform(std::move(shipping_option->prices_), convert_labeled_price));
}

static tl_object_ptr<telegram_api::paymentRequestedInfo> convert_order_info(
    tl_object_ptr<td_api::orderInfo> order_info) {
  if (order_info == nullptr) {
    return nullptr;
  }
  int32 flags = 0;
  if (!order_info->name_.empty()) {
    flags |= telegram_api::paymentRequestedInfo::NAME_MASK;
  }
  if (!order_info->phone_number_.empty()) {
    flags |= telegram_api::paymentRequestedInfo::PHONE_MASK;
  }
  if (!order_info->email_address_.empty()) {
    flags |= telegram_api::paymentRequestedInfo::EMAIL_MASK;
  }
  if (order_info->shipping_address_ != nullptr) {
    flags |= telegram_api::paymentRequestedInfo::SHIPPING_ADDRESS_MASK;
  }
  return make_tl_object<telegram_api::paymentRequestedInfo>(
      flags, std::move(order_info->name_), std::move(order_info->phone_number_), std::move(order_info->email_address_),
      convert_address(std::move(order_info->shipping_address_)));
}

static vector<tl_object_ptr<td_api::savedCredentials>> convert_saved_credentials(
    vector<tl_object_ptr<telegram_api::paymentSavedCredentialsCard>> saved_credentials) {
  return transform(
      std::move(saved_credentials), [](tl_object_ptr<telegram_api::paymentSavedCredentialsCard> &&credentials) {
        return make_tl_object<td_api::savedCredentials>(std::move(credentials->id_), std::move(credentials->title_));
      });
}

class GetPaymentFormQuery final : public Td::ResultHandler {
  Promise<tl_object_ptr<td_api::paymentForm>> promise_;
  DialogId dialog_id_;

 public:
  explicit GetPaymentFormQuery(Promise<tl_object_ptr<td_api::paymentForm>> &&promise) : promise_(std::move(promise)) {
  }

  void send(InputInvoiceInfo &&input_invoice_info, tl_object_ptr<telegram_api::dataJSON> &&theme_parameters) {
    dialog_id_ = input_invoice_info.dialog_id_;

    int32 flags = 0;
    if (theme_parameters != nullptr) {
      flags |= telegram_api::payments_getPaymentForm::THEME_PARAMS_MASK;
    }
    send_query(G()->net_query_creator().create(telegram_api::payments_getPaymentForm(
        flags, std::move(input_invoice_info.input_invoice_), std::move(theme_parameters))));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::payments_getPaymentForm>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto payment_form = result_ptr.move_as_ok();
    LOG(INFO) << "Receive result for GetPaymentFormQuery: " << to_string(payment_form);

    td_->contacts_manager_->on_get_users(std::move(payment_form->users_), "GetPaymentFormQuery");

    UserId payments_provider_user_id(payment_form->provider_id_);
    if (!payments_provider_user_id.is_valid()) {
      LOG(ERROR) << "Receive invalid payments provider " << payments_provider_user_id;
      return on_error(Status::Error(500, "Receive invalid payments provider identifier"));
    }
    UserId seller_bot_user_id(payment_form->bot_id_);
    if (!seller_bot_user_id.is_valid()) {
      LOG(ERROR) << "Receive invalid seller " << seller_bot_user_id;
      return on_error(Status::Error(500, "Receive invalid seller identifier"));
    }
    bool can_save_credentials = payment_form->can_save_credentials_;
    bool need_password = payment_form->password_missing_;
    auto photo = get_web_document_photo(td_->file_manager_.get(), std::move(payment_form->photo_), dialog_id_);
    auto payment_provider =
        convert_payment_provider(payment_form->native_provider_, std::move(payment_form->native_params_));
    if (payment_provider == nullptr) {
      payment_provider = td_api::make_object<td_api::paymentProviderOther>(std::move(payment_form->url_));
    }
    auto additional_payment_options = transform(
        payment_form->additional_methods_, [](const telegram_api::object_ptr<telegram_api::paymentFormMethod> &method) {
          return td_api::make_object<td_api::paymentOption>(method->title_, method->url_);
        });
    promise_.set_value(make_tl_object<td_api::paymentForm>(
        payment_form->form_id_, convert_invoice(std::move(payment_form->invoice_)),
        td_->contacts_manager_->get_user_id_object(seller_bot_user_id, "paymentForm seller"),
        td_->contacts_manager_->get_user_id_object(payments_provider_user_id, "paymentForm provider"),
        std::move(payment_provider), std::move(additional_payment_options),
        convert_order_info(std::move(payment_form->saved_info_)),
        convert_saved_credentials(std::move(payment_form->saved_credentials_)), can_save_credentials, need_password,
        payment_form->title_, get_product_description_object(payment_form->description_),
        get_photo_object(td_->file_manager_.get(), photo)));
  }

  void on_error(Status status) final {
    td_->messages_manager_->on_get_dialog_error(dialog_id_, status, "GetPaymentFormQuery");
    promise_.set_error(std::move(status));
  }
};

class ValidateRequestedInfoQuery final : public Td::ResultHandler {
  Promise<tl_object_ptr<td_api::validatedOrderInfo>> promise_;
  DialogId dialog_id_;

 public:
  explicit ValidateRequestedInfoQuery(Promise<tl_object_ptr<td_api::validatedOrderInfo>> &&promise)
      : promise_(std::move(promise)) {
  }

  void send(InputInvoiceInfo &&input_invoice_info, tl_object_ptr<telegram_api::paymentRequestedInfo> requested_info,
            bool allow_save) {
    dialog_id_ = input_invoice_info.dialog_id_;

    int32 flags = 0;
    if (allow_save) {
      flags |= telegram_api::payments_validateRequestedInfo::SAVE_MASK;
    }
    if (requested_info == nullptr) {
      requested_info = make_tl_object<telegram_api::paymentRequestedInfo>();
      requested_info->flags_ = 0;
    }
    send_query(G()->net_query_creator().create(telegram_api::payments_validateRequestedInfo(
        flags, false /*ignored*/, std::move(input_invoice_info.input_invoice_), std::move(requested_info))));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::payments_validateRequestedInfo>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto validated_order_info = result_ptr.move_as_ok();
    LOG(INFO) << "Receive result for ValidateRequestedInfoQuery: " << to_string(validated_order_info);

    promise_.set_value(make_tl_object<td_api::validatedOrderInfo>(
        std::move(validated_order_info->id_),
        transform(std::move(validated_order_info->shipping_options_), convert_shipping_option)));
  }

  void on_error(Status status) final {
    td_->messages_manager_->on_get_dialog_error(dialog_id_, status, "ValidateRequestedInfoQuery");
    promise_.set_error(std::move(status));
  }
};

class SendPaymentFormQuery final : public Td::ResultHandler {
  Promise<tl_object_ptr<td_api::paymentResult>> promise_;
  DialogId dialog_id_;

 public:
  explicit SendPaymentFormQuery(Promise<tl_object_ptr<td_api::paymentResult>> &&promise)
      : promise_(std::move(promise)) {
  }

  void send(InputInvoiceInfo &&input_invoice_info, int64 payment_form_id, const string &order_info_id,
            const string &shipping_option_id, tl_object_ptr<telegram_api::InputPaymentCredentials> input_credentials,
            int64 tip_amount) {
    CHECK(input_credentials != nullptr);

    dialog_id_ = input_invoice_info.dialog_id_;

    int32 flags = 0;
    if (!order_info_id.empty()) {
      flags |= telegram_api::payments_sendPaymentForm::REQUESTED_INFO_ID_MASK;
    }
    if (!shipping_option_id.empty()) {
      flags |= telegram_api::payments_sendPaymentForm::SHIPPING_OPTION_ID_MASK;
    }
    if (tip_amount != 0) {
      flags |= telegram_api::payments_sendPaymentForm::TIP_AMOUNT_MASK;
    }
    send_query(G()->net_query_creator().create(telegram_api::payments_sendPaymentForm(
        flags, payment_form_id, std::move(input_invoice_info.input_invoice_), order_info_id, shipping_option_id,
        std::move(input_credentials), tip_amount)));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::payments_sendPaymentForm>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto payment_result = result_ptr.move_as_ok();
    LOG(INFO) << "Receive result for SendPaymentFormQuery: " << to_string(payment_result);

    switch (payment_result->get_id()) {
      case telegram_api::payments_paymentResult::ID: {
        auto result = move_tl_object_as<telegram_api::payments_paymentResult>(payment_result);
        td_->updates_manager_->on_get_updates(
            std::move(result->updates_), PromiseCreator::lambda([promise = std::move(promise_)](Unit) mutable {
              promise.set_value(make_tl_object<td_api::paymentResult>(true, string()));
            }));
        return;
      }
      case telegram_api::payments_paymentVerificationNeeded::ID: {
        auto result = move_tl_object_as<telegram_api::payments_paymentVerificationNeeded>(payment_result);
        promise_.set_value(make_tl_object<td_api::paymentResult>(false, std::move(result->url_)));
        return;
      }
      default:
        UNREACHABLE();
    }
  }

  void on_error(Status status) final {
    td_->messages_manager_->on_get_dialog_error(dialog_id_, status, "SendPaymentFormQuery");
    promise_.set_error(std::move(status));
  }
};

class GetPaymentReceiptQuery final : public Td::ResultHandler {
  Promise<tl_object_ptr<td_api::paymentReceipt>> promise_;
  DialogId dialog_id_;

 public:
  explicit GetPaymentReceiptQuery(Promise<tl_object_ptr<td_api::paymentReceipt>> &&promise)
      : promise_(std::move(promise)) {
  }

  void send(DialogId dialog_id, ServerMessageId server_message_id) {
    dialog_id_ = dialog_id;
    auto input_peer = td_->messages_manager_->get_input_peer(dialog_id, AccessRights::Read);
    if (input_peer == nullptr) {
      return on_error(Status::Error(400, "Can't access the chat"));
    }

    send_query(G()->net_query_creator().create(
        telegram_api::payments_getPaymentReceipt(std::move(input_peer), server_message_id.get())));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::payments_getPaymentReceipt>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto payment_receipt = result_ptr.move_as_ok();
    LOG(INFO) << "Receive result for GetPaymentReceiptQuery: " << to_string(payment_receipt);

    td_->contacts_manager_->on_get_users(std::move(payment_receipt->users_), "GetPaymentReceiptQuery");

    UserId payments_provider_user_id(payment_receipt->provider_id_);
    if (!payments_provider_user_id.is_valid()) {
      LOG(ERROR) << "Receive invalid payments provider " << payments_provider_user_id;
      return on_error(Status::Error(500, "Receive invalid payments provider identifier"));
    }
    UserId seller_bot_user_id(payment_receipt->bot_id_);
    if (!seller_bot_user_id.is_valid()) {
      LOG(ERROR) << "Receive invalid seller " << seller_bot_user_id;
      return on_error(Status::Error(500, "Receive invalid seller identifier"));
    }
    auto photo = get_web_document_photo(td_->file_manager_.get(), std::move(payment_receipt->photo_), dialog_id_);
    if (payment_receipt->tip_amount_ < 0 || !check_currency_amount(payment_receipt->tip_amount_)) {
      LOG(ERROR) << "Receive invalid tip amount " << payment_receipt->tip_amount_;
      payment_receipt->tip_amount_ = 0;
    }

    promise_.set_value(make_tl_object<td_api::paymentReceipt>(
        payment_receipt->title_, get_product_description_object(payment_receipt->description_),
        get_photo_object(td_->file_manager_.get(), photo), payment_receipt->date_,
        td_->contacts_manager_->get_user_id_object(seller_bot_user_id, "paymentReceipt seller"),
        td_->contacts_manager_->get_user_id_object(payments_provider_user_id, "paymentReceipt provider"),
        convert_invoice(std::move(payment_receipt->invoice_)), convert_order_info(std::move(payment_receipt->info_)),
        convert_shipping_option(std::move(payment_receipt->shipping_)), std::move(payment_receipt->credentials_title_),
        payment_receipt->tip_amount_));
  }

  void on_error(Status status) final {
    td_->messages_manager_->on_get_dialog_error(dialog_id_, status, "GetPaymentReceiptQuery");
    promise_.set_error(std::move(status));
  }
};

class GetSavedInfoQuery final : public Td::ResultHandler {
  Promise<tl_object_ptr<td_api::orderInfo>> promise_;

 public:
  explicit GetSavedInfoQuery(Promise<tl_object_ptr<td_api::orderInfo>> &&promise) : promise_(std::move(promise)) {
  }

  void send() {
    send_query(G()->net_query_creator().create(telegram_api::payments_getSavedInfo()));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::payments_getSavedInfo>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto saved_info = result_ptr.move_as_ok();
    LOG(INFO) << "Receive result for GetSavedInfoQuery: " << to_string(saved_info);
    promise_.set_value(convert_order_info(std::move(saved_info->saved_info_)));
  }

  void on_error(Status status) final {
    promise_.set_error(std::move(status));
  }
};

class ClearSavedInfoQuery final : public Td::ResultHandler {
  Promise<Unit> promise_;

 public:
  explicit ClearSavedInfoQuery(Promise<Unit> &&promise) : promise_(std::move(promise)) {
  }

  void send(bool clear_credentials, bool clear_order_info) {
    CHECK(clear_credentials || clear_order_info);
    int32 flags = 0;
    if (clear_credentials) {
      flags |= telegram_api::payments_clearSavedInfo::CREDENTIALS_MASK;
    }
    if (clear_order_info) {
      flags |= telegram_api::payments_clearSavedInfo::INFO_MASK;
    }
    send_query(G()->net_query_creator().create(
        telegram_api::payments_clearSavedInfo(flags, false /*ignored*/, false /*ignored*/)));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::payments_clearSavedInfo>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    promise_.set_value(Unit());
  }

  void on_error(Status status) final {
    promise_.set_error(std::move(status));
  }
};

class ExportInvoiceQuery final : public Td::ResultHandler {
  Promise<string> promise_;

 public:
  explicit ExportInvoiceQuery(Promise<string> &&promise) : promise_(std::move(promise)) {
  }

  void send(tl_object_ptr<telegram_api::inputMediaInvoice> &&input_media_invoice) {
    send_query(G()->net_query_creator().create(telegram_api::payments_exportInvoice(std::move(input_media_invoice))));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::payments_exportInvoice>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto link = result_ptr.move_as_ok();
    promise_.set_value(std::move(link->url_));
  }

  void on_error(Status status) final {
    promise_.set_error(std::move(status));
  }
};

class GetBankCardInfoQuery final : public Td::ResultHandler {
  Promise<td_api::object_ptr<td_api::bankCardInfo>> promise_;

 public:
  explicit GetBankCardInfoQuery(Promise<td_api::object_ptr<td_api::bankCardInfo>> &&promise)
      : promise_(std::move(promise)) {
  }

  void send(const string &bank_card_number) {
    send_query(G()->net_query_creator().create(telegram_api::payments_getBankCardData(bank_card_number), {},
                                               G()->get_webfile_dc_id()));
  }

  void on_result(BufferSlice packet) final {
    auto result_ptr = fetch_result<telegram_api::payments_getBankCardData>(packet);
    if (result_ptr.is_error()) {
      return on_error(result_ptr.move_as_error());
    }

    auto response = result_ptr.move_as_ok();
    auto actions = transform(response->open_urls_, [](auto &open_url) {
      return td_api::make_object<td_api::bankCardActionOpenUrl>(open_url->name_, open_url->url_);
    });
    promise_.set_value(td_api::make_object<td_api::bankCardInfo>(response->title_, std::move(actions)));
  }

  void on_error(Status status) final {
    promise_.set_error(std::move(status));
  }
};

void answer_shipping_query(Td *td, int64 shipping_query_id,
                           vector<tl_object_ptr<td_api::shippingOption>> &&shipping_options,
                           const string &error_message, Promise<Unit> &&promise) {
  vector<tl_object_ptr<telegram_api::shippingOption>> options;
  for (auto &option : shipping_options) {
    if (option == nullptr) {
      return promise.set_error(Status::Error(400, "Shipping option must be non-empty"));
    }
    if (!clean_input_string(option->id_)) {
      return promise.set_error(Status::Error(400, "Shipping option identifier must be encoded in UTF-8"));
    }
    if (!clean_input_string(option->title_)) {
      return promise.set_error(Status::Error(400, "Shipping option title must be encoded in UTF-8"));
    }

    vector<tl_object_ptr<telegram_api::labeledPrice>> prices;
    for (auto &price_part : option->price_parts_) {
      if (price_part == nullptr) {
        return promise.set_error(Status::Error(400, "Shipping option price part must be non-empty"));
      }
      if (!clean_input_string(price_part->label_)) {
        return promise.set_error(Status::Error(400, "Shipping option price part label must be encoded in UTF-8"));
      }
      if (!check_currency_amount(price_part->amount_)) {
        return promise.set_error(Status::Error(400, "Too big amount of the currency specified"));
      }

      prices.push_back(make_tl_object<telegram_api::labeledPrice>(std::move(price_part->label_), price_part->amount_));
    }

    options.push_back(make_tl_object<telegram_api::shippingOption>(std::move(option->id_), std::move(option->title_),
                                                                   std::move(prices)));
  }

  td->create_handler<SetBotShippingAnswerQuery>(std::move(promise))
      ->send(shipping_query_id, error_message, std::move(options));
}

void answer_pre_checkout_query(Td *td, int64 pre_checkout_query_id, const string &error_message,
                               Promise<Unit> &&promise) {
  td->create_handler<SetBotPreCheckoutAnswerQuery>(std::move(promise))->send(pre_checkout_query_id, error_message);
}

void get_payment_form(Td *td, td_api::object_ptr<td_api::InputInvoice> &&input_invoice,
                      const td_api::object_ptr<td_api::themeParameters> &theme,
                      Promise<tl_object_ptr<td_api::paymentForm>> &&promise) {
  TRY_RESULT_PROMISE(promise, input_invoice_info, get_input_invoice_info(td, std::move(input_invoice)));

  tl_object_ptr<telegram_api::dataJSON> theme_parameters;
  if (theme != nullptr) {
    theme_parameters = make_tl_object<telegram_api::dataJSON>(string());
    theme_parameters->data_ = ThemeManager::get_theme_parameters_json_string(theme, false);
  }
  td->create_handler<GetPaymentFormQuery>(std::move(promise))
      ->send(std::move(input_invoice_info), std::move(theme_parameters));
}

void validate_order_info(Td *td, td_api::object_ptr<td_api::InputInvoice> &&input_invoice,
                         td_api::object_ptr<td_api::orderInfo> &&order_info, bool allow_save,
                         Promise<td_api::object_ptr<td_api::validatedOrderInfo>> &&promise) {
  TRY_RESULT_PROMISE(promise, input_invoice_info, get_input_invoice_info(td, std::move(input_invoice)));

  if (order_info != nullptr) {
    if (!clean_input_string(order_info->name_)) {
      return promise.set_error(Status::Error(400, "Name must be encoded in UTF-8"));
    }
    if (!clean_input_string(order_info->phone_number_)) {
      return promise.set_error(Status::Error(400, "Phone number must be encoded in UTF-8"));
    }
    if (!clean_input_string(order_info->email_address_)) {
      return promise.set_error(Status::Error(400, "Email address must be encoded in UTF-8"));
    }
    if (order_info->shipping_address_ != nullptr) {
      if (!clean_input_string(order_info->shipping_address_->country_code_)) {
        return promise.set_error(Status::Error(400, "Country code must be encoded in UTF-8"));
      }
      if (!clean_input_string(order_info->shipping_address_->state_)) {
        return promise.set_error(Status::Error(400, "State must be encoded in UTF-8"));
      }
      if (!clean_input_string(order_info->shipping_address_->city_)) {
        return promise.set_error(Status::Error(400, "City must be encoded in UTF-8"));
      }
      if (!clean_input_string(order_info->shipping_address_->street_line1_)) {
        return promise.set_error(Status::Error(400, "Street address must be encoded in UTF-8"));
      }
      if (!clean_input_string(order_info->shipping_address_->street_line2_)) {
        return promise.set_error(Status::Error(400, "Street address must be encoded in UTF-8"));
      }
      if (!clean_input_string(order_info->shipping_address_->postal_code_)) {
        return promise.set_error(Status::Error(400, "Postal code must be encoded in UTF-8"));
      }
    }
  }

  td->create_handler<ValidateRequestedInfoQuery>(std::move(promise))
      ->send(std::move(input_invoice_info), convert_order_info(std::move(order_info)), allow_save);
}

void send_payment_form(Td *td, td_api::object_ptr<td_api::InputInvoice> &&input_invoice, int64 payment_form_id,
                       const string &order_info_id, const string &shipping_option_id,
                       const td_api::object_ptr<td_api::InputCredentials> &credentials, int64 tip_amount,
                       Promise<td_api::object_ptr<td_api::paymentResult>> &&promise) {
  TRY_RESULT_PROMISE(promise, input_invoice_info, get_input_invoice_info(td, std::move(input_invoice)));

  if (credentials == nullptr) {
    return promise.set_error(Status::Error(400, "Input payment credentials must be non-empty"));
  }

  tl_object_ptr<telegram_api::InputPaymentCredentials> input_credentials;
  switch (credentials->get_id()) {
    case td_api::inputCredentialsSaved::ID: {
      auto credentials_saved = static_cast<const td_api::inputCredentialsSaved *>(credentials.get());
      auto credentials_id = credentials_saved->saved_credentials_id_;
      if (!clean_input_string(credentials_id)) {
        return promise.set_error(Status::Error(400, "Credentials identifier must be encoded in UTF-8"));
      }
      auto temp_password_state = PasswordManager::get_temp_password_state_sync();
      if (!temp_password_state.has_temp_password) {
        return promise.set_error(Status::Error(400, "Temporary password required to use saved credentials"));
      }

      input_credentials = make_tl_object<telegram_api::inputPaymentCredentialsSaved>(
          std::move(credentials_id), BufferSlice(temp_password_state.temp_password));
      break;
    }
    case td_api::inputCredentialsNew::ID: {
      auto credentials_new = static_cast<const td_api::inputCredentialsNew *>(credentials.get());
      int32 flags = 0;
      if (credentials_new->allow_save_) {
        flags |= telegram_api::inputPaymentCredentials::SAVE_MASK;
      }

      input_credentials = make_tl_object<telegram_api::inputPaymentCredentials>(
          flags, false /*ignored*/, make_tl_object<telegram_api::dataJSON>(credentials_new->data_));
      break;
    }
    case td_api::inputCredentialsGooglePay::ID: {
      auto credentials_google_pay = static_cast<const td_api::inputCredentialsGooglePay *>(credentials.get());
      input_credentials = make_tl_object<telegram_api::inputPaymentCredentialsGooglePay>(
          make_tl_object<telegram_api::dataJSON>(credentials_google_pay->data_));
      break;
    }
    case td_api::inputCredentialsApplePay::ID: {
      auto credentials_apple_pay = static_cast<const td_api::inputCredentialsApplePay *>(credentials.get());
      input_credentials = make_tl_object<telegram_api::inputPaymentCredentialsApplePay>(
          make_tl_object<telegram_api::dataJSON>(credentials_apple_pay->data_));
      break;
    }
    default:
      UNREACHABLE();
  }

  td->create_handler<SendPaymentFormQuery>(std::move(promise))
      ->send(std::move(input_invoice_info), payment_form_id, order_info_id, shipping_option_id,
             std::move(input_credentials), tip_amount);
}

void get_payment_receipt(Td *td, FullMessageId full_message_id,
                         Promise<tl_object_ptr<td_api::paymentReceipt>> &&promise) {
  TRY_RESULT_PROMISE(promise, server_message_id,
                     td->messages_manager_->get_payment_successful_message_id(full_message_id));
  td->create_handler<GetPaymentReceiptQuery>(std::move(promise))
      ->send(full_message_id.get_dialog_id(), server_message_id);
}

void get_saved_order_info(Td *td, Promise<tl_object_ptr<td_api::orderInfo>> &&promise) {
  td->create_handler<GetSavedInfoQuery>(std::move(promise))->send();
}

void delete_saved_order_info(Td *td, Promise<Unit> &&promise) {
  td->create_handler<ClearSavedInfoQuery>(std::move(promise))->send(false, true);
}

void delete_saved_credentials(Td *td, Promise<Unit> &&promise) {
  td->create_handler<ClearSavedInfoQuery>(std::move(promise))->send(true, false);
}

void export_invoice(Td *td, td_api::object_ptr<td_api::InputMessageContent> &&invoice, Promise<string> &&promise) {
  if (invoice == nullptr) {
    return promise.set_error(Status::Error(400, "Invoice must be non-empty"));
  }
  TRY_RESULT_PROMISE(promise, input_invoice,
                     InputInvoice::process_input_message_invoice(std::move(invoice), td, DialogId(), false));
  auto input_media = input_invoice.get_input_media_invoice(td, nullptr, nullptr);
  CHECK(input_media != nullptr);
  td->create_handler<ExportInvoiceQuery>(std::move(promise))->send(std::move(input_media));
}

void get_bank_card_info(Td *td, const string &bank_card_number,
                        Promise<td_api::object_ptr<td_api::bankCardInfo>> &&promise) {
  td->create_handler<GetBankCardInfoQuery>(std::move(promise))->send(bank_card_number);
}

}  // namespace td
