/*
This file is part of Telegram Desktop,
the official desktop application for the Telegram messaging service.

For license and copyright information please follow this link:
https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
*/
#include "settings/settings_scale_preview.h"

#include "base/platform/base_platform_info.h"
#include "base/event_filter.h"
#include "data/data_user.h"
#include "data/data_peer_values.h"
#include "history/history_item_components.h"
#include "main/main_session.h"
#include "ui/chat/chat_theme.h"
#include "ui/image/image_prepare.h"
#include "ui/platform/ui_platform_utility.h"
#include "ui/text/text_options.h"
#include "ui/widgets/shadow.h"
#include "ui/cached_round_corners.h"
#include "ui/painter.h"
#include "window/themes/window_theme.h"
#include "window/section_widget.h"
#include "window/window_controller.h"
#include "window/window_session_controller.h"
#include "styles/style_chat.h"

#include <QtGui/QGuiApplication>
#include <QtGui/QScreen>

namespace Settings {
namespace {

constexpr auto kMinTextWidth = 120;
constexpr auto kMaxTextWidth = 320;
constexpr auto kMaxTextLines = 3;

class Preview final {
public:
	Preview(QWidget *parent, rpl::producer<QImage> userpic);

	void toggle(ScalePreviewShow show, int scale, int globalX);

private:
	void init();
	void initAsWindow();

	void updateToScale(int scale);
	void updateGlobalPosition(int globalX);
	void updateGlobalPosition();
	void updateWindowGlobalPosition(QPoint global, int globalX);
	void updateOuterPosition(int globalX);
	[[nodiscard]] QRect adjustByScreenGeometry(QRect geometry) const;

	void toggleShown(bool shown);
	void toggleFilter();
	void update();

	void paint(Painter &p, QRect clip);
	void paintLayer(Painter &p, QRect clip);
	void paintInner(Painter &p, QRect clip);
	void paintUserpic(Painter &p, QRect clip);
	void paintBubble(Painter &p, QRect clip);
	void paintContent(Painter &p, QRect clip);
	void paintReply(Painter &p, QRect clip);
	void paintMessage(Painter &p, QRect clip);

	void validateUserpicCache();
	void validateBubbleCache();
	void validateShadowCache();

	[[nodiscard]] int scaled(int value) const;
	[[nodiscard]] QPoint scaled(QPoint value) const;
	[[nodiscard]] QSize scaled(QSize value) const;
	[[nodiscard]] QRect scaled(QRect value) const;
	[[nodiscard]] QMargins scaled(QMargins value) const;
	[[nodiscard]] style::font scaled(
		const style::font &value, int size) const;
	[[nodiscard]] style::TextStyle scaled(
		const style::TextStyle &value,
		int fontSize,
		int lineHeight) const;
	[[nodiscard]] QImage scaled(
		const style::icon &icon,
		const QColor &color) const;

	Ui::RpWidget _widget;
	Ui::ChatTheme _theme;
	style::TextStyle _nameStyle = st::fwdTextStyle;
	Ui::Text::String _nameText = { kMaxTextWidth / 3 };
	style::TextStyle _textStyle = st::messageTextStyle;
	Ui::Text::String _replyText = { kMaxTextWidth / 3 };
	Ui::Text::String _messageText = { kMaxTextWidth / 3 };
	style::Shadow _shadow = st::callShadow;
	std::array<QImage, 4> _shadowSides;
	std::array<QImage, 4> _shadowCorners;
	Ui::CornersPixmaps _bubbleCorners;
	QPixmap _bubbleShadowBottomRight;
	int _bubbleShadow = 0;
	int _localShiftLeft = 0;
	QImage _bubbleTail;
	QRect _replyBar;
	QRect _name;
	QRect _reply;
	QRect _message;
	QRect _content;
	QRect _bubble;
	QRect _userpic;
	QRect _inner;
	QRect _outer;
	QSize _minOuterSize;
	QSize _maxOuterSize;
	QImage _layer, _canvas;
	QPoint _cursor;
	std::array<QImage, 4> _canvasCornerMasks;
	QImage _userpicOriginal;
	QImage _userpicImage;
	int _scale = 0;
	int _ratio = 0;
	bool _window = false;

	Ui::Animations::Simple _shownAnimation;
	bool _shown = false;

	std::unique_ptr<QObject> _filter;

};

[[nodiscard]] bool UseSeparateWindow() {
	return !Platform::IsWayland()
		&& Ui::Platform::TranslucentWindowsSupported();
}

Preview::Preview(QWidget *parent, rpl::producer<QImage> userpic)
: _widget(parent)
, _ratio(style::DevicePixelRatio())
, _window(UseSeparateWindow()) {
	std::move(userpic) | rpl::start_with_next([=](QImage &&userpic) {
		_userpicOriginal = std::move(userpic);
		if (!_userpicImage.isNull()) {
			_userpicImage = {};
			update();
		}
	}, _widget.lifetime());

	init();
}

void Preview::toggle(ScalePreviewShow show, int scale, int globalX) {
	if (show == ScalePreviewShow::Hide) {
		toggleShown(false);
		return;
	} else if (show == ScalePreviewShow::Update && !_shown) {
		return;
	}
	updateToScale(scale);
	updateGlobalPosition(globalX);
	if (_widget.isHidden()) {
		Ui::Platform::UpdateOverlayed(&_widget);
	}
	toggleShown(true);
}

void Preview::toggleShown(bool shown) {
	if (_shown == shown) {
		return;
	}
	_shown = shown;
	toggleFilter();
	if (_shown) {
		_widget.show();
	} else if (_widget.isHidden()) {
		_shownAnimation.stop();
		return;
	}
	const auto callback = [=] {
		update();
		if (!_shown && !_shownAnimation.animating()) {
			_widget.hide();
		}
	};
	_shownAnimation.start(
		callback,
		shown ? 0. : 1.,
		shown ? 1. : 0.,
		st::slideWrapDuration);
}

void Preview::toggleFilter() {
	if (!_shown) {
		_filter = nullptr;
		return;
	} else if (_filter) {
		return;
	}
	_filter = std::make_unique<QObject>();
	const auto watch = [&](QWidget *widget, const auto &self) -> void {
		if (!widget) {
			return;
		}
		base::install_event_filter(_filter.get(), widget, [=](
				not_null<QEvent*> e) {
			if (e->type() == QEvent::Move
				|| e->type() == QEvent::Resize
				|| e->type() == QEvent::Show
				|| e->type() == QEvent::ShowToParent
				|| e->type() == QEvent::ZOrderChange) {
				updateGlobalPosition();
			}
			return base::EventFilterResult::Continue;
		});
		if (!_window && widget == _widget.window()) {
			return;
		}
		self(widget->parentWidget(), self);
	};
	watch(_widget.parentWidget(), watch);

	const auto checkDeactivation = [=](Qt::ApplicationState state) {
		if (state != Qt::ApplicationActive) {
			toggle(ScalePreviewShow::Hide, 0, 0);
		}
	};
	QObject::connect(
		qApp,
		&QGuiApplication::applicationStateChanged,
		_filter.get(),
		checkDeactivation,
		Qt::QueuedConnection);
}

void Preview::update() {
	_widget.update(_outer);
}

void Preview::init() {
	const auto background = Window::Theme::Background();
	const auto &paper = background->paper();
	_theme.setBackground({
		.prepared = background->prepared(),
		.preparedForTiled = background->preparedForTiled(),
		.gradientForFill = background->gradientForFill(),
		.colorForFill = background->colorForFill(),
		.colors = paper.backgroundColors(),
		.patternOpacity = paper.patternOpacity(),
		.gradientRotation = paper.gradientRotation(),
		.isPattern = paper.isPattern(),
		.tile = background->tile(),
	});

	_widget.paintRequest(
	) | rpl::start_with_next([=](QRect clip) {
		auto p = Painter(&_widget);
		paint(p, clip);
	}, _widget.lifetime());

	style::PaletteChanged(
	) | rpl::start_with_next([=] {
		_bubbleCorners = {};
		_bubbleTail = {};
		_bubbleShadowBottomRight = {};
		update();
	}, _widget.lifetime());

	if (_window) {
		initAsWindow();
		updateToScale(style::kScaleMin);
		_minOuterSize = _outer.size();
		updateToScale(style::kScaleMax / _ratio);
		_maxOuterSize = _outer.size();
	}
}

int Preview::scaled(int value) const {
	return style::ConvertScale(value, _scale);
}

QPoint Preview::scaled(QPoint value) const {
	return { scaled(value.x()), scaled(value.y()) };
}

QSize Preview::scaled(QSize value) const {
	return { scaled(value.width()), scaled(value.height()) };
}

QRect Preview::scaled(QRect value) const {
	return { scaled(value.topLeft()), scaled(value.size()) };
}

QMargins Preview::scaled(QMargins value) const {
	return {
		scaled(value.left()),
		scaled(value.top()),
		scaled(value.right()),
		scaled(value.bottom()),
	};
}

style::font Preview::scaled(const style::font &font, int size) const {
	return style::font(scaled(size), font->flags(), font->family());
}

style::TextStyle Preview::scaled(
		const style::TextStyle &value,
		int fontSize,
		int lineHeight) const {
	return {
		.font = scaled(value.font, fontSize),
		.linkFont = scaled(value.linkFont, fontSize),
		.linkFontOver = scaled(value.linkFontOver, fontSize),
		.lineHeight = scaled(value.lineHeight),
	};
}

QImage Preview::scaled(
		const style::icon &icon,
		const QColor &color) const {
	return icon.instance(color, _scale);
}

void Preview::updateToScale(int scale) {
	using style::ConvertScale;

	if (_scale == scale) {
		return;
	}
	_scale = scale;
	_nameStyle = scaled(_nameStyle, 13, 0);
	_textStyle = scaled(_textStyle, 13, 0);
	_nameText.setText(
		_nameStyle,
		u"Bob Harris"_q,
		Ui::NameTextOptions());
	_replyText.setText(
		_textStyle,
		u"Good morning!"_q,
		Ui::ItemTextDefaultOptions());
	_messageText.setText(
		_textStyle,
		u"Do you know what time it is?"_q,
		Ui::ItemTextDefaultOptions());

	_replyBar = QRect(
		scaled(1), // st::msgReplyBarPos.x(),
		scaled(6) + 0,// st::msgReplyPadding.top() + st::msgReplyBarPos.y(),
		scaled(2), //st::msgReplyBarSize.width(),
		scaled(36)); // st::msgReplyBarSize.height(),
	const auto namePosition = QPoint(
		scaled(10), // st::msgReplyBarSkip
		scaled(6)); // st::msgReplyPadding.top()
	const auto replyPosition = QPoint(
		scaled(10), // st::msgReplyBarSkip
		scaled(6) + _nameStyle.font->height); // st::msgReplyPadding.top()

	const auto wantedWidth = std::max({
		namePosition.x() + _nameText.maxWidth(),
		replyPosition.x() + _replyText.maxWidth(),
		_messageText.maxWidth(),
	});

	const auto minTextWidth = scaled(kMinTextWidth);
	const auto maxTextWidth = scaled(kMaxTextWidth);
	const auto messageWidth = std::clamp(
		wantedWidth,
		minTextWidth,
		maxTextWidth);
	const auto messageHeight = std::min(
		_messageText.countHeight(maxTextWidth),
		kMaxTextLines * _textStyle.font->height);

	_name = QRect(
		namePosition,
		QSize(messageWidth - namePosition.x(), _nameStyle.font->height));
	_reply = QRect(
		replyPosition,
		QSize(messageWidth - replyPosition.x(), _textStyle.font->height));
	_message = QRect(0, 0, messageWidth, messageHeight);

	// replyBar.bottom + st::msgReplyPadding.bottom();
	const auto replySkip = _replyBar.y() + _replyBar.height() + scaled(6);
	_message.moveTop(replySkip);

	_content = QRect(0, 0, messageWidth, replySkip + messageHeight);

	const auto msgPadding = scaled(QMargins(13, 7, 13, 8)); // st::msgPadding
	_bubble = _content.marginsAdded(msgPadding);
	_content.moveTopLeft(-_bubble.topLeft());
	_bubble.moveTopLeft({});
	_bubbleShadow = scaled(2); // st::msgShadow
	_bubbleCorners = {};
	_bubbleTail = {};
	_bubbleShadowBottomRight = {};

	const auto hasUserpic = !_userpicOriginal.isNull();
	const auto bubbleMargin = scaled(QMargins(20, 16, 20, 16));
	const auto userpicSkip = hasUserpic ? scaled(40) : 0; // st::msgPhotoSkip
	_inner = _bubble.marginsAdded(
		bubbleMargin + QMargins(userpicSkip, 0, 0, 0));
	_bubble.moveTopLeft(-_inner.topLeft());
	_inner.moveTopLeft({});
	if (hasUserpic) {
		const auto userpicSize = scaled(33); // st::msgPhotoSize
		_userpic = QRect(
			bubbleMargin.left(),
			_bubble.y() + _bubble.height() - userpicSize,
			userpicSize,
			userpicSize);
		_userpicImage = {};
	}

	_shadow.extend = scaled(QMargins(9, 8, 9, 10)); // st::callShadow.extend
	_shadowSides = {};
	_shadowCorners = {};

	update();
	_outer = _inner.marginsAdded(_shadow.extend);
	_inner.moveTopLeft(-_outer.topLeft());
	_outer.moveTopLeft({});

	_layer = QImage(
		_outer.size() * _ratio,
		QImage::Format_ARGB32_Premultiplied);
	_layer.setDevicePixelRatio(_ratio);
	_canvas = QImage(
		_inner.size() * _ratio,
		QImage::Format_ARGB32_Premultiplied);
	_canvas.setDevicePixelRatio(_ratio);
	_canvas.fill(Qt::transparent);

	_canvasCornerMasks = Images::CornersMask(scaled(6)); // st::callRadius
}

void Preview::updateGlobalPosition(int globalX) {
	const auto parent = _widget.parentWidget();
	if (_window) {
		const auto global = parent->mapToGlobal(QPoint());
		_localShiftLeft = globalX - global.x();
		updateWindowGlobalPosition(global, globalX);
	} else {
		const auto position = parent->pos();
			+ QPoint(parent->width() / 2, 0)
			- QPoint(_outer.width() / 2, _outer.height());
		_widget.setGeometry(QRect(position, _outer.size()));
		updateOuterPosition(globalX);
	}
}

void Preview::updateGlobalPosition() {
	const auto parent = _widget.parentWidget();
	const auto global = parent->mapToGlobal(QPoint());
	updateWindowGlobalPosition(global, global.x() + _localShiftLeft);
}

void Preview::updateWindowGlobalPosition(QPoint global, int globalX) {
	const auto desiredLeft = global.x() - (_minOuterSize.width() / 2);
	const auto desiredRight = global.x()
		+ _widget.parentWidget()->width()
		+ (_maxOuterSize.width() / 2);
	const auto requiredLeft = desiredRight - _maxOuterSize.width();
	const auto left = std::min(desiredLeft, requiredLeft);
	const auto requiredRight = left + _maxOuterSize.width();
	const auto right = std::max(desiredRight, requiredRight);
	const auto top = global.y() - _maxOuterSize.height();
	auto result = QRect(left, top, right - left, _maxOuterSize.height());
	_widget.setGeometry(adjustByScreenGeometry(result));
	updateOuterPosition(globalX);
}

QRect Preview::adjustByScreenGeometry(QRect geometry) const {
	const auto parent = _widget.parentWidget();
	const auto screen = parent->screen();
	if (!screen) {
		return geometry;
	}
	const auto screenGeometry = screen->availableGeometry();
	if (!screenGeometry.intersects(geometry)
		|| screenGeometry.width() < _maxOuterSize.width()
		|| screenGeometry.height() < _maxOuterSize.height()) {
		return geometry;
	}
	const auto edgeLeft = screenGeometry.x();
	const auto edgeRight = screenGeometry.x() + screenGeometry.width();
	const auto edgedRight = std::min(
		edgeRight,
		geometry.x() + geometry.width());
	const auto left = std::max(
		std::min(geometry.x(), edgedRight - _maxOuterSize.width()),
		edgeLeft);
	const auto right = std::max(edgedRight, left + _maxOuterSize.width());
	return { left, geometry.y(), right - left, geometry.height() };
}

void Preview::updateOuterPosition(int globalX) {
	if (_window) {
		update();
		const auto global = _widget.geometry();
		const auto desiredLeft = globalX
			- (_outer.width() / 2)
			- global.x();
		_outer.moveLeft(std::max(
			std::min(desiredLeft, global.width() - _outer.width()),
			0));
		_outer.moveTop(_maxOuterSize.height() - _outer.height());
		update();
	}
}

void Preview::paint(Painter &p, QRect clip) {
	//p.setCompositionMode(QPainter::CompositionMode_Source);
	//p.fillRect(clip, Qt::transparent);
	//p.setCompositionMode(QPainter::CompositionMode_SourceOver);

	const auto outer = clip.intersected(_outer);
	if (outer.isEmpty()) {
		return;
	}
	const auto local = outer.translated(-_outer.topLeft());
	auto q = Painter(&_layer);
	q.setClipRect(local);
	paintLayer(q, local);
	q.end();

	const auto shown = _shownAnimation.value(_shown ? 1. : 0.);
	p.setClipRect(clip);
	p.setOpacity(shown);
	auto hq = std::optional<PainterHighQualityEnabler>();
	if (shown < 1.) {
		const auto middle = _outer.x() + (_outer.width() / 2);
		const auto bottom = _outer.y() + _outer.height();
		const auto scale = 0.3 + shown * 0.7;
		p.translate(middle, bottom);
		p.scale(scale, scale);
		p.translate(-middle, -bottom);
		hq.emplace(p);
	}
	p.drawImage(_outer.topLeft(), _layer);
}

void Preview::paintLayer(Painter &p, QRect clip) {
	p.setCompositionMode(QPainter::CompositionMode_Source);
	validateShadowCache();
	Ui::Shadow::paint(
		p,
		_inner,
		_outer.width(),
		_shadow,
		_shadowSides,
		_shadowCorners);

	const auto inner = clip.intersected(_inner);
	if (inner.isEmpty()) {
		return;
	}
	const auto local = inner.translated(-_inner.topLeft());
	auto q = Painter(&_canvas);
	q.setClipRect(local);
	paintInner(q, local);
	q.end();
	_canvas = Images::Round(std::move(_canvas), _canvasCornerMasks);

	p.setCompositionMode(QPainter::CompositionMode_SourceOver);
	p.drawImage(_inner.topLeft(), _canvas);
}

void Preview::paintInner(Painter &p, QRect clip) {
	Window::SectionWidget::PaintBackground(
		p,
		&_theme,
		QSize(_inner.width(), _inner.width() * 3),
		clip);

	paintUserpic(p, clip);

	p.translate(_bubble.topLeft());
	paintBubble(p, clip.translated(-_bubble.topLeft()));
}

void Preview::paintUserpic(Painter &p, QRect clip) {
	if (clip.intersected(_userpic).isEmpty()) {
		return;
	}
	validateUserpicCache();
	p.drawImage(_userpic.topLeft(), _userpicImage);
}

void Preview::paintBubble(Painter &p, QRect clip) {
	validateBubbleCache();
	const auto bubble = QRect(QPoint(), _bubble.size());
	const auto cornerShadow = _bubbleShadowBottomRight.size()
		/ _bubbleShadowBottomRight.devicePixelRatio();
	p.drawPixmap(
		bubble.width() - cornerShadow.width(),
		bubble.height() + _bubbleShadow - cornerShadow.height(),
		_bubbleShadowBottomRight);
	Ui::FillRoundRect(p, bubble, st::msgInBg, _bubbleCorners);
	const auto tail = _bubbleTail.size() / _bubbleTail.devicePixelRatio();
	p.drawImage(-tail.width(), bubble.height() - tail.height(), _bubbleTail);
	p.fillRect(
		-tail.width(),
		bubble.height(),
		tail.width() + bubble.width() - cornerShadow.width(),
		_bubbleShadow,
		st::msgInShadow);

	const auto content = clip.intersected(_content);
	if (content.isEmpty()) {
		return;
	}
	p.translate(_content.topLeft());
	const auto local = content.translated(-_content.topLeft());
	p.setClipRect(local);
	paintContent(p, local);
}

void Preview::paintContent(Painter &p, QRect clip) {
	paintReply(p, clip);

	const auto message = clip.intersected(_message);
	if (message.isEmpty()) {
		return;
	}
	p.translate(_message.topLeft());
	const auto local = message.translated(-_message.topLeft());
	p.setClipRect(local);
	paintMessage(p, local);
}

void Preview::paintReply(Painter &p, QRect clip) {
	p.setOpacity(HistoryMessageReply::kBarAlpha);
	p.fillRect(_replyBar, st::msgInReplyBarColor);
	p.setOpacity(1.);

	p.setPen(st::msgInServiceFg);
	_nameText.drawLeftElided(
		p,
		_name.x(),
		_name.y(),
		_name.width(),
		_content.width());

	p.setPen(st::historyTextInFg);
	_replyText.drawLeftElided(
		p,
		_reply.x(),
		_reply.y(),
		_reply.width(),
		_content.width());
}

void Preview::paintMessage(Painter &p, QRect clip) {
	p.setPen(st::historyTextInFg);
	_messageText.drawLeftElided(
		p,
		0,
		0,
		_message.width(),
		_message.width(),
		kMaxTextLines);
}

void Preview::validateUserpicCache() {
	if (!_userpicImage.isNull()
		|| _userpicOriginal.isNull()
		|| _userpic.isEmpty()) {
		return;
	}
	_userpicImage = Images::Circle(_userpicOriginal.scaled(
		_userpic.size() * _ratio,
		Qt::IgnoreAspectRatio,
		Qt::SmoothTransformation));
	_userpicImage.setDevicePixelRatio(_ratio);
}

void Preview::validateBubbleCache() {
	if (!_bubbleCorners.p[0].isNull()) {
		return;
	}
	const auto radius = scaled(16); // st::bubbleRadiusLarge
	_bubbleCorners = Ui::PrepareCornerPixmaps(radius, st::msgInBg);
	_bubbleCorners.p[2] = {};
	_bubbleTail = scaled(st::historyBubbleTailInLeft, st::msgInBg->c);
	_bubbleShadowBottomRight
		= Ui::PrepareCornerPixmaps(radius, st::msgInShadow).p[3];
}

void Preview::validateShadowCache() {
	if (!_shadowSides[0].isNull()) {
		return;
	}
	const auto &shadowColor = st::windowShadowFg->c;
	_shadowSides[0] = scaled(st::callShadow.left, shadowColor);
	_shadowSides[1] = scaled(st::callShadow.top, shadowColor);
	_shadowSides[2] = scaled(st::callShadow.right, shadowColor);
	_shadowSides[3] = scaled(st::callShadow.bottom, shadowColor);
	_shadowCorners[0] = scaled(st::callShadow.topLeft, shadowColor);
	_shadowCorners[1] = scaled(st::callShadow.bottomLeft, shadowColor);
	_shadowCorners[2] = scaled(st::callShadow.topRight, shadowColor);
	_shadowCorners[3] = scaled(st::callShadow.bottomRight, shadowColor);
}

void Preview::initAsWindow() {
	_widget.setWindowFlags(Qt::WindowFlags(Qt::FramelessWindowHint)
		| Qt::BypassWindowManagerHint
		| Qt::NoDropShadowWindowHint
		| Qt::ToolTip);
	_widget.setAttribute(Qt::WA_TransparentForMouseEvents);
	_widget.hide();

	_widget.setAttribute(Qt::WA_NoSystemBackground);
	_widget.setAttribute(Qt::WA_TranslucentBackground);
}

} // namespace

[[nodiscard]] Fn<void(ScalePreviewShow, int, int)> SetupScalePreview(
		not_null<Window::Controller*> window,
		not_null<Ui::RpWidget*> slider) {
	const auto parent = slider->parentWidget();
	const auto controller = window->sessionController();
	const auto user = controller
		? controller->session().user().get()
		: nullptr;
	auto view = user->activeUserpicView();
	const auto preview = slider->lifetime().make_state<Preview>(
		slider.get(),
		user ? Data::PeerUserpicImageValue(user, 160, 0) : nullptr);
	return [=](ScalePreviewShow show, int scale, int globalX) {
		preview->toggle(show, scale, globalX);
	};
}

} // namespace Settings