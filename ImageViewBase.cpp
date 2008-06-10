/*
    Scan Tailor - Interactive post-processing tool for scanned pages.
    Copyright (C) 2007-2008  Joseph Artsimovich <joseph_a@mail.ru>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "ImageViewBase.h.moc"
#include "PixmapRenderer.h"
#include "Dpm.h"
#include "Dpi.h"
#include <QPainter>
#include <QBrush>
#include <QLineF>
#include <QMouseEvent>
#include <QWheelEvent>
#include <algorithm>
#include <math.h>
#include <assert.h>

ImageViewBase::ImageViewBase(QImage const& image)
:	m_pixmap(QPixmap::fromImage(image)),
	m_physToVirt(image.rect(), Dpm(image)),
	m_focalPoint(0.5 * image.width(), 0.5 * image.height()),
	m_zoom(1.0),
	m_currentCursorShape(Qt::ArrowCursor),
	m_isDraggingInProgress(false)
{
	updateWidgetTransform();
}

ImageViewBase::ImageViewBase(
	QImage const& image, ImageTransformation const& pre_transform)
:	m_pixmap(QPixmap::fromImage(image)),
	m_physToVirt(pre_transform),
	m_focalPoint(0.5 * image.width(), 0.5 * image.height()),
	m_zoom(1.0),
	m_currentCursorShape(Qt::ArrowCursor),
	m_isDraggingInProgress(false)
{
	updateWidgetTransformAndFixFocalPoint();
}

ImageViewBase::~ImageViewBase()
{
}

void
ImageViewBase::paintEvent(QPaintEvent* const event)
{
	double const xscale = m_virtualToWidget.m11();
	
	QPainter painter(this);
	
	// Width of a source pixel in mm, as it's displayed on screen.
	double const pixel_width = widthMM() * xscale / width();
#if !defined(Q_WS_X11)
	// On X11 SmoothPixmapTransform is too slow.
	painter.setRenderHint(QPainter::SmoothPixmapTransform, pixel_width < 0.5);
#endif
	painter.setWorldTransform(m_physToVirt.transform() * m_virtualToWidget);
	
	PixmapRenderer::drawPixmap(painter, m_pixmap);
	
	painter.setWorldTransform(m_virtualToWidget);
	
	paintOverImage(painter);
}

void
ImageViewBase::paintOverImage(QPainter& painter)
{
}

void
ImageViewBase::resizeEvent(QResizeEvent*)
{
	updateWidgetTransform();
}

void
ImageViewBase::hideEvent(QHideEvent*)
{
	m_isDraggingInProgress = false;
}

void
ImageViewBase::handleZooming(QWheelEvent* const event)
{
	if (m_isDraggingInProgress) {
		return;
	}
	
	double const degrees = event->delta() / 8.0;
	m_zoom *= pow(2.0, degrees / 60.0); // 2 times zoom for every 60 degrees
	if (m_zoom < 1.0) {
		m_zoom = 1.0;
	}
	
	updateWidgetTransform();
	update();
}

void
ImageViewBase::handleImageDragging(QMouseEvent* const event)
{
	switch (event->type()) {
		case QEvent::MouseButtonPress:
			if (!m_isDraggingInProgress && event->button() == Qt::LeftButton) {
				m_lastMousePos = event->pos();
				m_isDraggingInProgress = true;
			}
			break;
		case QEvent::MouseButtonRelease:
			if (m_isDraggingInProgress && event->button() == Qt::LeftButton) {
				m_isDraggingInProgress= false;
			}
			break;
		case QEvent::MouseMove:
			if (m_isDraggingInProgress) {
				QPoint movement(event->pos());
				movement -= m_lastMousePos;
				m_lastMousePos = event->pos();
				
				// Map the focal point to widget coordinates.
				QTransform xform(m_physToVirt.transform() * m_virtualToWidget);
				QPointF widget_focal_point(xform.map(m_focalPoint));
				
				// Move the focal point in the opposite direction
				// of the mouse movement direction.
				widget_focal_point -= movement;
				
				// Map it back to image coordinates;
				xform = m_widgetToVirtual * m_physToVirt.transformBack();
				QPointF const new_focal_point(xform.map(widget_focal_point));
				
				adjustAndSetNewFocalPoint(new_focal_point);
				update();
			}
		default:;
	}
}

bool
ImageViewBase::isDraggingPossible() const
{
	QRectF const image_rect(m_physToVirt.resultingRect());
	QRectF const widget_rect(m_virtualToWidget.mapRect(image_rect));
	if (widget_rect.top() <= -1.0) {
		return true;
	}
	if (widget_rect.left() <= -1.0) {
		return true;
	}
	if (widget_rect.bottom() >= height() + 1) {
		return true;
	}
	if (widget_rect.right() >= width() + 1) {
		return true;
	}
	return false;
}

QRectF
ImageViewBase::getVisibleWidgetRect() const
{
	QRectF const image_rect(m_physToVirt.resultingRect());
	QRectF const widget_rect(m_virtualToWidget.mapRect(image_rect));
	return widget_rect.intersected(rect());
}

void
ImageViewBase::updateTransform(ImageTransformation const& phys_to_virt)
{
	m_physToVirt = phys_to_virt;
	updateWidgetTransform();
	update();
}

void
ImageViewBase::updateTransformAndFixFocalPoint(
	ImageTransformation const& phys_to_virt)
{
	m_physToVirt = phys_to_virt;
	updateWidgetTransformAndFixFocalPoint();
	update();
}

void
ImageViewBase::updateTransformPreservingScale(
	ImageTransformation const& phys_to_virt)
{
	// An arbitrary line in physical coordinates.
	QLineF const phys_line(0.0, 0.0, 1.0, 1.0);
	
	QLineF const widget_line_before(
		m_virtualToWidget.map(m_physToVirt.transform().map(phys_line))
	);
	
	m_physToVirt = phys_to_virt;
	updateWidgetTransform();
	
	QLineF const widget_line_after(
		m_virtualToWidget.map(m_physToVirt.transform().map(phys_line))
	);
	
	m_zoom *= widget_line_before.length() / widget_line_after.length();
	updateWidgetTransform();
	
	update();
}

void
ImageViewBase::ensureCursorShape(Qt::CursorShape const cursor_shape)
{
	if (cursor_shape != m_currentCursorShape) {
		m_currentCursorShape = cursor_shape;
		setCursor(cursor_shape);
	}
}

/**
 * Updates m_virtualToWidget and m_widgetToVirtual.\n
 * To be called whenever m_physToVirt or m_focalPoint are modified.
 */
void
ImageViewBase::updateWidgetTransform()
{
	QRectF const virt_rect(m_physToVirt.resultingRect());
	QPointF const virt_origin(m_physToVirt.transform().map(m_focalPoint));
	QPointF const widget_origin(0.5 * width(), 0.5 * height());
	
	QSizeF zoom1_widget_size(virt_rect.size());
	zoom1_widget_size.scale(size(), Qt::KeepAspectRatio);
	
	double const zoom1_x = zoom1_widget_size.width() / virt_rect.width();
	double const zoom1_y = zoom1_widget_size.height() / virt_rect.height();
	
	QTransform t1;
	t1.translate(-virt_origin.x(), -virt_origin.y());
	
	QTransform t2;
	t2.scale(zoom1_x * m_zoom, zoom1_y * m_zoom);
	
	QTransform t3;
	t3.translate(widget_origin.x(), widget_origin.y());
	
	m_virtualToWidget = t1 * t2 * t3;
	m_widgetToVirtual = m_virtualToWidget.inverted();
}

/**
 * Updates m_virtualToWidget and m_widgetToVirtual and adjusts
 * the focal point if necessary.\n
 * To be called whenever m_physToVirt is modified in such a way that
 * may invalidate the focal point.
 */
void
ImageViewBase::updateWidgetTransformAndFixFocalPoint()
{
	updateWidgetTransform();
	
	QPointF const ideal_focal_point(getIdealFocalPoint());
	if (ideal_focal_point != m_focalPoint) {
		m_focalPoint = ideal_focal_point;
		// Move the image so that the new focal point
		// is at the center of the widget.
		updateWidgetTransform();
	}
}

/**
 * An ideal focal point is an adjustment to the current focal point,
 * which ensures that no widget space is wasted.  For example if the
 * focal point is at the corner of an image, the image would cover
 * at most 1/4 of the widget space, which is a waste.\n
 * If some widget space has to be wasted because the image is smaller
 * than the widget (maybe just in one direction), the focal point
 * is adjusted to center the image at that direction.
 * In case the image already covers the whole widget, the ideal
 * focal point is the current focal point.
 */
QPointF
ImageViewBase::getIdealFocalPoint() const
{
	QRectF const viewport(m_virtualToWidget.mapRect(m_physToVirt.resultingRect()));
	double const left_margin = viewport.left();
	double const right_margin = width() - viewport.right();
	double const top_margin = viewport.top();
	double const bottom_margin = height() - viewport.bottom();
	
	// The focal point in widget coordinates.
	QPointF widget_focal_point(0.5 * width(), 0.5 * height());
	
	if (left_margin + right_margin >= 0.0) {
		// Image fits horizontally, so center it in that direction.
		widget_focal_point.setX(viewport.left() + 0.5 * viewport.width());
	} else if (left_margin < 0.0 && right_margin > 0.0) {
		// Move image to the right so that either left_margin or
		// right_margin becomes zero, whichever requires less movement.
		double const movement = std::min(fabs(left_margin), fabs(right_margin));
		widget_focal_point.rx() -= movement;
	} else if (right_margin < 0.0 && left_margin > 0.0) {
		// Move image to the left so that either left_margin or
		// right_margin becomes zero, whichever requires less movement.
		double const movement = std::min(fabs(left_margin), fabs(right_margin));
		widget_focal_point.rx() += movement;
	}
	
	if (top_margin + bottom_margin >= 0.0) {
		// Image fits vertically, so center it in that direction.
		widget_focal_point.setY(viewport.top() + 0.5 * viewport.height());
	} else if (top_margin < 0.0 && bottom_margin > 0.0) {
		// Move image down so that either top_margin or bottom_margin
		// becomes zero, whichever requires less movement.
		double const movement = std::min(fabs(top_margin), fabs(bottom_margin));
		widget_focal_point.ry() -= movement;
	} else if (bottom_margin < 0.0 && top_margin > 0.0) {
		// Move image up so that either top_margin or bottom_margin
		// becomes zero, whichever requires less movement.
		double const movement = std::min(fabs(top_margin), fabs(bottom_margin));
		widget_focal_point.ry() += movement;
	}
	
	QTransform const xform(m_widgetToVirtual * m_physToVirt.transformBack());
	return xform.map(widget_focal_point);
}

/**
 * Used when dragging the image.  It adjusts the movement to disallow
 * dragging it away from the ideal position (determined by getIdealFocalPoint()).
 */
void
ImageViewBase::adjustAndSetNewFocalPoint(QPointF const new_focal_point)
{
	QPointF const old_focal_point(m_focalPoint);
	m_focalPoint = new_focal_point;
	updateWidgetTransform();
	QPointF const ideal_focal_point(getIdealFocalPoint());
	
	QPointF adjusted_focal_point(old_focal_point);
	
	if (old_focal_point.x() < ideal_focal_point.x()) {
		if (old_focal_point.x() < new_focal_point.x()) {
			adjusted_focal_point.setX(
				std::min(new_focal_point.x(), ideal_focal_point.x())
			);
		}
	} else {
		if (new_focal_point.x() < old_focal_point.x()) {
			adjusted_focal_point.setX(
				std::max(new_focal_point.x(), ideal_focal_point.x())
			);
		}
	}
	
	if (old_focal_point.y() < ideal_focal_point.y()) {
		if (old_focal_point.y() < new_focal_point.y()) {
			adjusted_focal_point.setY(
				std::min(new_focal_point.y(), ideal_focal_point.y())
			);
		}
	} else {
		if (new_focal_point.y() < old_focal_point.y()) {
			adjusted_focal_point.setY(
				std::max(new_focal_point.y(), ideal_focal_point.y())
			);
		}
	}
	
	if (adjusted_focal_point != m_focalPoint) {
		m_focalPoint = adjusted_focal_point;
		// Move the image so that the new focal point
		// is at the center of the widget.
		updateWidgetTransform();
	}
}