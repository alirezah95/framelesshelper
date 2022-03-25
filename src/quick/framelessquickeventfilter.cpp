/*
 * MIT License
 *
 * Copyright (C) 2022 by wangwenx190 (Yuhang Zhao)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "framelessquickeventfilter.h"
#include <QtCore/qmutex.h>
#include <QtCore/qhash.h>
#include <QtCore/qlist.h>
#include <QtQuick/qquickwindow.h>
#include <QtQuick/qquickitem.h>
#include <utils.h>

FRAMELESSHELPER_BEGIN_NAMESPACE

using namespace Global;

struct EventFilterDataInternal
{
    FramelessQuickEventFilter *eventFilter = nullptr;
    QQuickItem *titleBarItem = nullptr;
    QList<QQuickItem *> hitTestVisibleItems = {};
    FramelessHelperParams params = {};
};

struct EventFilterData
{
    QMutex mutex = {};
    QHash<WId, EventFilterDataInternal> data = {};
};

Q_GLOBAL_STATIC(EventFilterData, g_data)

[[nodiscard]] static inline bool isInTitleBarDraggableArea(QQuickWindow *window, const QPoint &pos)
{
    Q_ASSERT(window);
    if (!window) {
        return false;
    }
    const WId windowId = window->winId();
    g_data()->mutex.lock();
    if (!g_data()->data.contains(windowId)) {
        g_data()->mutex.unlock();
        return false;
    }
    const EventFilterDataInternal data = g_data()->data.value(windowId);
    g_data()->mutex.unlock();
    if (!data.titleBarItem) {
        return false;
    }
    const auto mapGeometryToScene = [](const QQuickItem * const item) -> QRect {
        Q_ASSERT(item);
        if (!item) {
            return {};
        }
        return QRect(item->mapToScene(QPointF(0.0, 0.0)).toPoint(), item->size().toSize());
    };
    QRegion region = mapGeometryToScene(data.titleBarItem);
    if (!data.hitTestVisibleItems.isEmpty()) {
        for (auto &&item : qAsConst(data.hitTestVisibleItems)) {
            Q_ASSERT(item);
            if (item) {
                region -= mapGeometryToScene(item);
            }
        }
    }
    return region.contains(pos);
}

FramelessQuickEventFilter::FramelessQuickEventFilter(QObject *parent) : QObject(parent) {}

FramelessQuickEventFilter::~FramelessQuickEventFilter() = default;

void FramelessQuickEventFilter::addWindow(const FramelessHelperParams &params)
{
    Q_ASSERT(params.isValid());
    if (!params.isValid()) {
        return;
    }
    g_data()->mutex.lock();
    if (g_data()->data.contains(params.windowId)) {
        g_data()->mutex.unlock();
        return;
    }
    auto data = EventFilterDataInternal{};
    data.params = params;
    const auto window = qobject_cast<QQuickWindow *>(params.getWindowHandle());
    // Give it a parent so that it can be deleted even if we forget to do so.
    data.eventFilter = new FramelessQuickEventFilter(window);
    g_data()->data.insert(params.windowId, data);
    g_data()->mutex.unlock();
    window->installEventFilter(data.eventFilter);
}

void FramelessQuickEventFilter::setTitleBarItem(QQuickWindow *window, QQuickItem *item)
{
    Q_ASSERT(window);
    Q_ASSERT(item);
    if (!window || !item) {
        return;
    }
    const WId windowId = window->winId();
    QMutexLocker locker(&g_data()->mutex);
    if (!g_data()->data.contains(windowId)) {
        return;
    }
    g_data()->data[windowId].titleBarItem = item;
}

void FramelessQuickEventFilter::setHitTestVisible(QQuickWindow *window, QQuickItem *item)
{
    Q_ASSERT(window);
    Q_ASSERT(item);
    if (!window || !item) {
        return;
    }
    const WId windowId = window->winId();
    QMutexLocker locker(&g_data()->mutex);
    if (!g_data()->data.contains(windowId)) {
        return;
    }
    auto &items = g_data()->data[windowId].hitTestVisibleItems;
    static constexpr const bool visible = true;
    const bool exists = items.contains(item);
    if (visible && !exists) {
        items.append(item);
    }
    if constexpr (!visible && exists) {
        items.removeAll(item);
    }
}

bool FramelessQuickEventFilter::eventFilter(QObject *object, QEvent *event)
{
    Q_ASSERT(object);
    Q_ASSERT(event);
    if (!object || !event) {
        return false;
    }
    if (!object->isWindowType()) {
        return false;
    }
    const auto window = qobject_cast<QQuickWindow *>(object);
    if (!window) {
        return false;
    }
    const WId windowId = window->winId();
    g_data()->mutex.lock();
    if (!g_data()->data.contains(windowId)) {
        g_data()->mutex.unlock();
        return false;
    }
    const EventFilterDataInternal data = g_data()->data.value(windowId);
    g_data()->mutex.unlock();
    const QEvent::Type eventType = event->type();
    if ((eventType != QEvent::MouseButtonPress) && (eventType != QEvent::MouseButtonRelease)
        && (eventType != QEvent::MouseButtonDblClick)) {
        return false;
    }
    const auto mouseEvent = static_cast<QMouseEvent *>(event);
    const Qt::MouseButton button = mouseEvent->button();
    if ((button != Qt::LeftButton) && (button != Qt::RightButton)) {
        return false;
    }
#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
    const QPoint scenePos = mouseEvent->scenePosition().toPoint();
#else
    const QPoint scenePos = mouseEvent->windowPos().toPoint();
#endif
    const QQuickWindow::Visibility visibility = window->visibility();
    if ((visibility == QQuickWindow::Windowed)
        && ((scenePos.x() < kDefaultResizeBorderThickness)
         || (scenePos.x() >= (window->width() - kDefaultResizeBorderThickness))
         || (scenePos.y() < kDefaultResizeBorderThickness))) {
        return false;
    }
    const bool titleBar = isInTitleBarDraggableArea(window, scenePos);
    const bool isFixedSize = data.params.isWindowFixedSize();
    switch (eventType) {
    case QEvent::MouseButtonPress: {
        if (data.params.options & Option::DisableDragging) {
            return false;
        }
        if (button != Qt::LeftButton) {
            return false;
        }
        if (!titleBar) {
            return false;
        }
        Utils::startSystemMove(window);
        return true;
    }
    case QEvent::MouseButtonRelease: {
        if (data.params.options & Option::DisableSystemMenu) {
            return false;
        }
        if (button != Qt::RightButton) {
            return false;
        }
        if (!titleBar) {
            return false;
        }
#if (QT_VERSION >= QT_VERSION_CHECK(6, 0, 0))
        const QPoint globalPos = mouseEvent->globalPosition().toPoint();
#else
        const QPoint globalPos = mouseEvent->globalPos();
#endif
        const QPoint nativePos = QPointF(QPointF(globalPos) * window->effectiveDevicePixelRatio()).toPoint();
        Utils::showSystemMenu(windowId, nativePos, data.params.options,
                              data.params.systemMenuOffset, data.params.isWindowFixedSize);
        return true;
    }
    case QEvent::MouseButtonDblClick: {
        if ((data.params.options & Option::NoDoubleClickMaximizeToggle) || isFixedSize) {
            return false;
        }
        if (button != Qt::LeftButton) {
            return false;
        }
        if (!titleBar) {
            return false;
        }
        if ((visibility == QQuickWindow::Maximized)
            || ((data.params.options & Option::DontTreatFullScreenAsZoomed)
                    ? false : (visibility == QQuickWindow::FullScreen))) {
            window->showNormal();
        } else {
            window->showMaximized();
        }
        return true;
    }
    default:
        break;
    }
    return false;
}

FRAMELESSHELPER_END_NAMESPACE