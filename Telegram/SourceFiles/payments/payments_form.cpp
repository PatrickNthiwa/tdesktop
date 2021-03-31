/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "payments/payments_form.h"

#include "main/main_session.h"
#include "data/data_session.h"
#include "data/data_media_types.h"
#include "data/data_user.h"
#include "data/data_photo.h"
#include "data/data_photo_media.h"
#include "data/data_file_origin.h"
#include "data/data_countries.h"
#include "history/history_item.h"
#include "history/history_service.h" // HistoryServicePayment.
#include "stripe/stripe_api_client.h"
#include "stripe/stripe_error.h"
#include "stripe/stripe_token.h"
#include "stripe/stripe_card_validator.h"
#include "ui/image/image.h"
#include "apiwrap.h"
#include "styles/style_payments.h" // paymentsThumbnailSize.

#include <QtCore/QJsonDocument>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonValue>

namespace Payments {
namespace {

[[nodiscard]] Ui::Address ParseAddress(const MTPPostAddress &address) {
	return address.match([](const MTPDpostAddress &data) {
		return Ui::Address{
			.address1 = qs(data.vstreet_line1()),
			.address2 = qs(data.vstreet_line2()),
			.city = qs(data.vcity()),
			.state = qs(data.vstate()),
			.countryIso2 = qs(data.vcountry_iso2()),
			.postcode = qs(data.vpost_code()),
		};
	});
}

[[nodiscard]] int64 ParsePriceAmount(uint64 value) {
	return *reinterpret_cast<const int64*>(&value);
}

[[nodiscard]] std::vector<Ui::LabeledPrice> ParsePrices(
		const MTPVector<MTPLabeledPrice> &data) {
	return ranges::views::all(
		data.v
	) | ranges::views::transform([](const MTPLabeledPrice &price) {
		return price.match([&](const MTPDlabeledPrice &data) {
			return Ui::LabeledPrice{
				.label = qs(data.vlabel()),
				.price = ParsePriceAmount(data.vamount().v),
			};
		});
	}) | ranges::to_vector;
}

[[nodiscard]] MTPPaymentRequestedInfo Serialize(
		const Ui::RequestedInformation &information) {
	using Flag = MTPDpaymentRequestedInfo::Flag;
	return MTP_paymentRequestedInfo(
		MTP_flags((information.name.isEmpty() ? Flag(0) : Flag::f_name)
			| (information.email.isEmpty() ? Flag(0) : Flag::f_email)
			| (information.phone.isEmpty() ? Flag(0) : Flag::f_phone)
			| (information.shippingAddress
				? Flag::f_shipping_address
				: Flag(0))),
		MTP_string(information.name),
		MTP_string(information.phone),
		MTP_string(information.email),
		MTP_postAddress(
			MTP_string(information.shippingAddress.address1),
			MTP_string(information.shippingAddress.address2),
			MTP_string(information.shippingAddress.city),
			MTP_string(information.shippingAddress.state),
			MTP_string(information.shippingAddress.countryIso2),
			MTP_string(information.shippingAddress.postcode)));
}

[[nodiscard]] QString CardTitle(const Stripe::Card &card) {
	// Like server stores saved_credentials title.
	return Stripe::CardBrandToString(card.brand()).toLower()
		+ " *"
		+ card.last4();
}

} // namespace

Form::Form(not_null<PeerData*> peer, MsgId itemId, bool receipt)
: _session(&peer->session())
, _api(&_session->mtp())
, _peer(peer)
, _msgId(itemId)
, _receiptMode(receipt) {
	fillInvoiceFromMessage();
	if (_receiptMode) {
		_invoice.receipt.paid = true;
		requestReceipt();
	} else {
		requestForm();
	}
}

Form::~Form() = default;

void Form::fillInvoiceFromMessage() {
	const auto id = FullMsgId(peerToChannel(_peer->id), _msgId);
	if (const auto item = _session->data().message(id)) {
		const auto media = [&] {
			if (const auto payment = item->Get<HistoryServicePayment>()) {
				if (payment->msg) {
					return payment->msg->media();
				}
			}
			return item->media();
		}();
		if (const auto invoice = media ? media->invoice() : nullptr) {
			_invoice.cover = Ui::Cover{
				.title = invoice->title,
				.description = invoice->description,
			};
			if (const auto photo = invoice->photo) {
				loadThumbnail(photo);
			}
		}
	}
}

void Form::loadThumbnail(not_null<PhotoData*> photo) {
	Expects(!_thumbnailLoadProcess);

	auto view = photo->createMediaView();
	if (auto good = prepareGoodThumbnail(view); !good.isNull()) {
		_invoice.cover.thumbnail = std::move(good);
		return;
	}
	_thumbnailLoadProcess = std::make_unique<ThumbnailLoadProcess>();
	if (auto blurred = prepareBlurredThumbnail(view); !blurred.isNull()) {
		_invoice.cover.thumbnail = std::move(blurred);
		_thumbnailLoadProcess->blurredSet = true;
	} else {
		_invoice.cover.thumbnail = prepareEmptyThumbnail();
	}
	_thumbnailLoadProcess->view = std::move(view);
	photo->load(
		Data::PhotoSize::Thumbnail,
		FullMsgId(peerToChannel(_peer->id), _msgId));
	_session->downloaderTaskFinished(
	) | rpl::start_with_next([=] {
		const auto &view = _thumbnailLoadProcess->view;
		if (auto good = prepareGoodThumbnail(view); !good.isNull()) {
			_invoice.cover.thumbnail = std::move(good);
			_thumbnailLoadProcess = nullptr;
		} else if (_thumbnailLoadProcess->blurredSet) {
			return;
		} else if (auto blurred = prepareBlurredThumbnail(view)
			; !blurred.isNull()) {
			_invoice.cover.thumbnail = std::move(blurred);
			_thumbnailLoadProcess->blurredSet = true;
		} else {
			return;
		}
		_updates.fire(ThumbnailUpdated{ _invoice.cover.thumbnail });
	}, _thumbnailLoadProcess->lifetime);
}

QImage Form::prepareGoodThumbnail(
		const std::shared_ptr<Data::PhotoMedia> &view) const {
	using Size = Data::PhotoSize;
	if (const auto large = view->image(Size::Large)) {
		return prepareThumbnail(large);
	} else if (const auto thumbnail = view->image(Size::Thumbnail)) {
		return prepareThumbnail(thumbnail);
	}
	return QImage();
}

QImage Form::prepareBlurredThumbnail(
		const std::shared_ptr<Data::PhotoMedia> &view) const {
	if (const auto small = view->image(Data::PhotoSize::Small)) {
		return prepareThumbnail(small, true);
	} else if (const auto blurred = view->thumbnailInline()) {
		return prepareThumbnail(blurred, true);
	}
	return QImage();
}

QImage Form::prepareThumbnail(
		not_null<const Image*> image,
		bool blurred) const {
	auto result = image->original().scaled(
		st::paymentsThumbnailSize * cIntRetinaFactor(),
		Qt::KeepAspectRatio,
		Qt::SmoothTransformation);
	Images::prepareRound(result, ImageRoundRadius::Large);
	result.setDevicePixelRatio(cRetinaFactor());
	return result;
}

QImage Form::prepareEmptyThumbnail() const {
	auto result = QImage(
		st::paymentsThumbnailSize * cIntRetinaFactor(),
		QImage::Format_ARGB32_Premultiplied);
	result.setDevicePixelRatio(cRetinaFactor());
	result.fill(Qt::transparent);
	return result;
}

void Form::requestForm() {
	_api.request(MTPpayments_GetPaymentForm(
		MTP_flags(0),
		_peer->input,
		MTP_int(_msgId),
		MTP_dataJSON(MTP_string(QString()))
	)).done([=](const MTPpayments_PaymentForm &result) {
		result.match([&](const auto &data) {
			processForm(data);
		});
	}).fail([=](const MTP::Error &error) {
		_updates.fire(Error{ Error::Type::Form, error.type() });
	}).send();
}

void Form::requestReceipt() {
	_api.request(MTPpayments_GetPaymentReceipt(
		_peer->input,
		MTP_int(_msgId)
	)).done([=](const MTPpayments_PaymentReceipt &result) {
		result.match([&](const auto &data) {
			processReceipt(data);
		});
	}).fail([=](const MTP::Error &error) {
		_updates.fire(Error{ Error::Type::Form, error.type() });
	}).send();
}

void Form::processForm(const MTPDpayments_paymentForm &data) {
	_session->data().processUsers(data.vusers());

	data.vinvoice().match([&](const auto &data) {
		processInvoice(data);
	});
	processDetails(data);
	if (const auto info = data.vsaved_info()) {
		info->match([&](const auto &data) {
			processSavedInformation(data);
		});
	}
	if (const auto credentials = data.vsaved_credentials()) {
		credentials->match([&](const auto &data) {
			processSavedCredentials(data);
		});
	}
	fillPaymentMethodInformation();
	_updates.fire(FormReady{});
}

void Form::processReceipt(const MTPDpayments_paymentReceipt &data) {
	_session->data().processUsers(data.vusers());

	data.vinvoice().match([&](const auto &data) {
		processInvoice(data);
	});
	processDetails(data);
	if (const auto info = data.vinfo()) {
		info->match([&](const auto &data) {
			processSavedInformation(data);
		});
	}
	if (const auto shipping = data.vshipping()) {
		processShippingOptions({ *shipping });
		if (!_shippingOptions.list.empty()) {
			_shippingOptions.selectedId = _shippingOptions.list.front().id;
		}
	}
	_paymentMethod.savedCredentials = SavedCredentials{
		.id = "(used)",
		.title = qs(data.vcredentials_title()),
	};
	fillPaymentMethodInformation();
	_updates.fire(FormReady{});
}

void Form::processInvoice(const MTPDinvoice &data) {
	_invoice = Ui::Invoice{
		.cover = std::move(_invoice.cover),

		.prices = ParsePrices(data.vprices()),
		.tipsMin = ParsePriceAmount(data.vmin_tip_amount().value_or_empty()),
		.tipsMax = ParsePriceAmount(data.vmax_tip_amount().value_or_empty()),
		.tipsSelected = ParsePriceAmount(
			data.vdefault_tip_amount().value_or_empty()),
		.currency = qs(data.vcurrency()),

		.isNameRequested = data.is_name_requested(),
		.isPhoneRequested = data.is_phone_requested(),
		.isEmailRequested = data.is_email_requested(),
		.isShippingAddressRequested = data.is_shipping_address_requested(),
		.isFlexible = data.is_flexible(),
		.isTest = data.is_test(),

		.phoneSentToProvider = data.is_phone_to_provider(),
		.emailSentToProvider = data.is_email_to_provider(),
	};
}

void Form::processDetails(const MTPDpayments_paymentForm &data) {
	const auto nativeParams = data.vnative_params();
	auto nativeParamsJson = nativeParams
		? nativeParams->match(
			[&](const MTPDdataJSON &data) { return data.vdata().v; })
		: QByteArray();
	_details = FormDetails{
		.formId = data.vform_id().v,
		.url = qs(data.vurl()),
		.nativeProvider = qs(data.vnative_provider().value_or_empty()),
		.nativeParamsJson = std::move(nativeParamsJson),
		.botId = data.vbot_id().v,
		.providerId = data.vprovider_id().v,
		.canSaveCredentials = data.is_can_save_credentials(),
		.passwordMissing = data.is_password_missing(),
	};
	if (_details.botId) {
		if (const auto bot = _session->data().userLoaded(_details.botId)) {
			_invoice.cover.seller = bot->name;
		}
	}
}

void Form::processDetails(const MTPDpayments_paymentReceipt &data) {
	_invoice.receipt = Ui::Receipt{
		.date = data.vdate().v,
		.totalAmount = ParsePriceAmount(data.vtotal_amount().v),
		.currency = qs(data.vcurrency()),
		.paid = true,
	};
	_details = FormDetails{
		.botId = data.vbot_id().v,
		.providerId = data.vprovider_id().v,
	};
	if (_details.botId) {
		if (const auto bot = _session->data().userLoaded(_details.botId)) {
			_invoice.cover.seller = bot->name;
		}
	}
}

void Form::processSavedInformation(const MTPDpaymentRequestedInfo &data) {
	const auto address = data.vshipping_address();
	_savedInformation = Ui::RequestedInformation{
		.defaultPhone = defaultPhone(),
		.defaultCountry = defaultCountry(),
		.name = qs(data.vname().value_or_empty()),
		.phone = qs(data.vphone().value_or_empty()),
		.email = qs(data.vemail().value_or_empty()),
		.shippingAddress = address ? ParseAddress(*address) : Ui::Address(),
	};
}

void Form::processSavedCredentials(
		const MTPDpaymentSavedCredentialsCard &data) {
	// #TODO payments save
	//_nativePayment.savedCredentials = SavedCredentials{
	//	.id = qs(data.vid()),
	//	.title = qs(data.vtitle()),
	//};
	refreshPaymentMethodDetails();
}

void Form::refreshPaymentMethodDetails() {
	const auto &saved = _paymentMethod.savedCredentials;
	const auto &entered = _paymentMethod.newCredentials;
	_paymentMethod.ui.title = entered ? entered.title : saved.title;
	_paymentMethod.ui.ready = entered || saved;
	_paymentMethod.ui.native.defaultCountry = defaultCountry();
}

QString Form::defaultPhone() const {
	return _session->user()->phone();
}

QString Form::defaultCountry() const {
	return Data::CountryISO2ByPhone(defaultPhone());
}

void Form::fillPaymentMethodInformation() {
	_paymentMethod.native = NativePaymentMethod();
	_paymentMethod.ui.native = Ui::NativeMethodDetails();
	_paymentMethod.ui.url = _details.url;
	if (_details.nativeProvider == "stripe") {
		fillStripeNativeMethod();
	}
	refreshPaymentMethodDetails();
}

void Form::fillStripeNativeMethod() {
	auto error = QJsonParseError();
	auto document = QJsonDocument::fromJson(
		_details.nativeParamsJson,
		&error);
	if (error.error != QJsonParseError::NoError) {
		LOG(("Payment Error: Could not decode native_params, error %1: %2"
			).arg(error.error
			).arg(error.errorString()));
		return;
	} else if (!document.isObject()) {
		LOG(("Payment Error: Not an object in native_params."));
		return;
	}
	const auto object = document.object();
	const auto value = [&](QStringView key) {
		return object.value(key);
	};
	const auto key = value(u"publishable_key").toString();
	if (key.isEmpty()) {
		LOG(("Payment Error: No publishable_key in native_params."));
		return;
	}
	_paymentMethod.native = NativePaymentMethod{
		.data = StripePaymentMethod{
			.publishableKey = key,
		},
	};
	_paymentMethod.ui.native = Ui::NativeMethodDetails{
		.supported = true,
		.needCountry = value(u"need_country").toBool(),
		.needZip = value(u"need_zip").toBool(),
		.needCardholderName = value(u"need_cardholder_name").toBool(),
	};
}

void Form::submit() {
	Expects(!_paymentMethod.newCredentials.data.isEmpty()); // #TODO payments save

	using Flag = MTPpayments_SendPaymentForm::Flag;
	_api.request(MTPpayments_SendPaymentForm(
		MTP_flags((_requestedInformationId.isEmpty()
			? Flag(0)
			: Flag::f_requested_info_id)
			| (_shippingOptions.selectedId.isEmpty()
				? Flag(0)
				: Flag::f_shipping_option_id)
			| (_invoice.tipsSelected ? Flag::f_tip_amount : Flag(0))),
		MTP_long(_details.formId),
		_peer->input,
		MTP_int(_msgId),
		MTP_string(_requestedInformationId),
		MTP_string(_shippingOptions.selectedId),
		MTP_inputPaymentCredentials(
			MTP_flags(0),
			MTP_dataJSON(MTP_bytes(_paymentMethod.newCredentials.data))),
		MTP_long(_invoice.tipsSelected)
	)).done([=](const MTPpayments_PaymentResult &result) {
		result.match([&](const MTPDpayments_paymentResult &data) {
			_updates.fire(PaymentFinished{ data.vupdates() });
		}, [&](const MTPDpayments_paymentVerificationNeeded &data) {
			_updates.fire(VerificationNeeded{ qs(data.vurl()) });
		});
	}).fail([=](const MTP::Error &error) {
		_updates.fire(Error{ Error::Type::Send, error.type() });
	}).send();
}

void Form::validateInformation(const Ui::RequestedInformation &information) {
	if (_validateRequestId) {
		if (_validatedInformation == information) {
			return;
		}
		_api.request(base::take(_validateRequestId)).cancel();
	}
	_validatedInformation = information;
	if (!validateInformationLocal(information)) {
		return;
	}

	Assert(!_invoice.isShippingAddressRequested
		|| information.shippingAddress);
	Assert(!_invoice.isNameRequested || !information.name.isEmpty());
	Assert(!_invoice.isEmailRequested || !information.email.isEmpty());
	Assert(!_invoice.isPhoneRequested || !information.phone.isEmpty());

	using Flag = MTPpayments_ValidateRequestedInfo::Flag;
	_validateRequestId = _api.request(MTPpayments_ValidateRequestedInfo(
		MTP_flags(information.save ? Flag::f_save : Flag(0)),
		_peer->input,
		MTP_int(_msgId),
		Serialize(information)
	)).done([=](const MTPpayments_ValidatedRequestedInfo &result) {
		_validateRequestId = 0;
		const auto oldSelectedId = _shippingOptions.selectedId;
		result.match([&](const MTPDpayments_validatedRequestedInfo &data) {
			_requestedInformationId = data.vid().value_or_empty();
			processShippingOptions(
				data.vshipping_options().value_or_empty());
		});
		_shippingOptions.selectedId = ranges::contains(
			_shippingOptions.list,
			oldSelectedId,
			&Ui::ShippingOption::id
		) ? oldSelectedId : QString();
		if (_shippingOptions.selectedId.isEmpty()
			&& _shippingOptions.list.size() == 1) {
			_shippingOptions.selectedId = _shippingOptions.list.front().id;
		}
		_savedInformation = _validatedInformation;
		_updates.fire(ValidateFinished{});
	}).fail([=](const MTP::Error &error) {
		_validateRequestId = 0;
		_updates.fire(Error{ Error::Type::Validate, error.type() });
	}).send();
}

bool Form::validateInformationLocal(
		const Ui::RequestedInformation &information) const {
	if (const auto error = informationErrorLocal(information)) {
		_updates.fire_copy(error);
		return false;
	}
	return true;
}

Error Form::informationErrorLocal(
		const Ui::RequestedInformation &information) const {
	auto errors = QStringList();
	const auto push = [&](const QString &id) {
		errors.push_back(id);
	};
	if (_invoice.isShippingAddressRequested) {
		if (information.shippingAddress.address1.isEmpty()) {
			push(u"ADDRESS_STREET_LINE1_INVALID"_q);
		}
		if (information.shippingAddress.city.isEmpty()) {
			push(u"ADDRESS_CITY_INVALID"_q);
		}
		if (information.shippingAddress.countryIso2.isEmpty()) {
			push(u"ADDRESS_COUNTRY_INVALID"_q);
		}
	}
	if (_invoice.isNameRequested && information.name.isEmpty()) {
		push(u"REQ_INFO_NAME_INVALID"_q);
	}
	if (_invoice.isEmailRequested && information.email.isEmpty()) {
		push(u"REQ_INFO_EMAIL_INVALID"_q);
	}
	if (_invoice.isPhoneRequested && information.phone.isEmpty()) {
		push(u"REQ_INFO_PHONE_INVALID"_q);
	}
	if (!errors.isEmpty()) {
		return Error{ Error::Type::Validate, errors.front() };
	}
	return Error();
}

void Form::validateCard(const Ui::UncheckedCardDetails &details) {
	Expects(!v::is_null(_paymentMethod.native.data));

	if (!validateCardLocal(details)) {
		return;
	}
	const auto &native = _paymentMethod.native.data;
	if (const auto stripe = std::get_if<StripePaymentMethod>(&native)) {
		validateCard(*stripe, details);
	} else {
		Unexpected("Native payment provider in Form::validateCard.");
	}
}

bool Form::validateCardLocal(const Ui::UncheckedCardDetails &details) const {
	if (auto error = cardErrorLocal(details)) {
		_updates.fire(std::move(error));
		return false;
	}
	return true;
}

Error Form::cardErrorLocal(const Ui::UncheckedCardDetails &details) const {
	using namespace Stripe;

	auto errors = QStringList();
	const auto push = [&](const QString &id) {
		errors.push_back(id);
	};
	const auto kValid = ValidationState::Valid;
	if (ValidateCard(details.number).state != kValid) {
		push(u"LOCAL_CARD_NUMBER_INVALID"_q);
	}
	if (ValidateParsedExpireDate(
		details.expireMonth,
		details.expireYear
	) != kValid) {
		push(u"LOCAL_CARD_EXPIRE_DATE_INVALID"_q);
	}
	if (ValidateCvc(details.number, details.cvc).state != kValid) {
		push(u"LOCAL_CARD_CVC_INVALID"_q);
	}
	if (_paymentMethod.ui.native.needCardholderName
		&& details.cardholderName.isEmpty()) {
		push(u"LOCAL_CARD_HOLDER_NAME_INVALID"_q);
	}
	if (_paymentMethod.ui.native.needCountry
		&& details.addressCountry.isEmpty()) {
		push(u"LOCAL_CARD_BILLING_COUNTRY_INVALID"_q);
	}
	if (_paymentMethod.ui.native.needZip
		&& details.addressZip.isEmpty()) {
		push(u"LOCAL_CARD_BILLING_ZIP_INVALID"_q);
	}
	if (!errors.isEmpty()) {
		return Error{ Error::Type::Validate, errors.front() };
	}
	return Error();
}

void Form::validateCard(
		const StripePaymentMethod &method,
		const Ui::UncheckedCardDetails &details) {
	Expects(!method.publishableKey.isEmpty());

	if (_stripe) {
		return;
	}
	auto configuration = Stripe::PaymentConfiguration{
		.publishableKey = method.publishableKey,
		.companyName = "Telegram",
	};
	_stripe = std::make_unique<Stripe::APIClient>(std::move(configuration));
	auto card = Stripe::CardParams{
		.number = details.number,
		.expMonth = details.expireMonth,
		.expYear = details.expireYear,
		.cvc = details.cvc,
		.name = details.cardholderName,
		.addressZip = details.addressZip,
		.addressCountry = details.addressCountry,
	};
	_stripe->createTokenWithCard(std::move(card), crl::guard(this, [=](
			Stripe::Token token,
			Stripe::Error error) {
		_stripe = nullptr;

		if (error) {
			LOG(("Stripe Error %1: %2 (%3)"
				).arg(int(error.code())
				).arg(error.description()
				).arg(error.message()));
			_updates.fire(Error{ Error::Type::Stripe, error.description() });
		} else {
			setPaymentCredentials({
				.title = CardTitle(token.card()),
				.data = QJsonDocument(QJsonObject{
					{ "type", "card" },
					{ "id", token.tokenId() },
				}).toJson(QJsonDocument::Compact),
				.saveOnServer = false, // #TODO payments save
			});
		}
	}));
}

void Form::setPaymentCredentials(const NewCredentials &credentials) {
	Expects(!credentials.empty());

	_paymentMethod.newCredentials = credentials;
	refreshPaymentMethodDetails();
	_updates.fire(PaymentMethodUpdate{});
}

void Form::setShippingOption(const QString &id) {
	_shippingOptions.selectedId = id;
}

void Form::setTips(int64 value) {
	_invoice.tipsSelected = std::clamp(
		value,
		_invoice.tipsMin,
		_invoice.tipsMax);
}

void Form::processShippingOptions(const QVector<MTPShippingOption> &data) {
	const auto currency = _invoice.currency;
	_shippingOptions = Ui::ShippingOptions{ currency, ranges::views::all(
		data
	) | ranges::views::transform([](const MTPShippingOption &option) {
		return option.match([](const MTPDshippingOption &data) {
			return Ui::ShippingOption{
				.id = qs(data.vid()),
				.title = qs(data.vtitle()),
				.prices = ParsePrices(data.vprices()),
			};
		});
	}) | ranges::to_vector };
	_shippingOptions.currency = _invoice.currency;
}

} // namespace Payments