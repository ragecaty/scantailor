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

#include "Task.h"
#include "Filter.h"
#include "OptionsWidget.h"
#include "Settings.h"
#include "Params.h"
#include "Dependencies.h"
#include "TaskStatus.h"
#include "PageLayout.h"
#include "DebugImages.h"
#include "filters/select_content/Task.h"
#include "FilterUiInterface.h"
#include "ImageView.h"
#include "FilterData.h"
#include "Dpi.h"
#include "Dpm.h"
#include "ImageTransformation.h"
#include "imageproc/BinaryImage.h"
#include "imageproc/BWColor.h"
#include "imageproc/OrthogonalRotation.h"
#include "imageproc/SkewFinder.h"
#include "imageproc/RasterOp.h"
#include "imageproc/ReduceThreshold.h"
#include "imageproc/ExpandPower2.h"
#include "imageproc/SeedFill.h"
#include "imageproc/Connectivity.h"
#include "imageproc/Morphology.h"
#include <QImage>
#include <QSize>
#include <QPoint>
#include <QRect>
#include <QPolygonF>
#include <QTransform>
#include <vector>
#include <memory>
#include <algorithm>
#include <assert.h>
#include <stddef.h>

namespace deskew
{

using namespace imageproc;

class Task::UiUpdater : public FilterResult
{
public:
	UiUpdater(IntrusivePtr<Filter> const& filter,
		std::auto_ptr<DebugImages> dbg_img,
		QImage const& image, LogicalPageId const& page_id,
		ImageTransformation const& xform,
		OptionsWidget::UiData const& ui_data);
	
	virtual void updateUI(FilterUiInterface* ui);
	
	virtual IntrusivePtr<AbstractFilter> filter() { return m_ptrFilter; }
private:
	IntrusivePtr<Filter> m_ptrFilter;
	std::auto_ptr<DebugImages> m_ptrDbg;
	QImage m_image;
	LogicalPageId m_pageId;
	ImageTransformation m_xform;
	OptionsWidget::UiData m_uiData;
};


Task::Task(IntrusivePtr<Filter> const& filter,
	IntrusivePtr<Settings> const& settings,
	IntrusivePtr<select_content::Task> const& next_task,
	LogicalPageId const& page_id, bool const debug)
:	m_ptrFilter(filter),
	m_ptrSettings(settings),
	m_ptrNextTask(next_task),
	m_pageId(page_id)
{
	if (debug) {
		m_ptrDbg.reset(new DebugImages);
	}
}

Task::~Task()
{
}

FilterResultPtr
Task::process(
	TaskStatus const& status, FilterData const& data,
	PageLayout const& page_layout)
{
	status.throwIfCancelled();
	
	QRectF const rect(data.xform().rectBeforeCropping());
	
	QPolygonF const page_outline(
		page_layout.pageOutline(rect, m_pageId.subPage())
	);
	Dependencies const deps(page_outline, data.xform().preRotation());
	
	OptionsWidget::UiData ui_data;
	ui_data.setDependencies(deps);
	
	std::auto_ptr<Params> params(m_ptrSettings->getPageParams(m_pageId));
	if (params.get()) {
		if (!deps.matches(params->dependencies())) {
			params.reset();
		} else {
			ui_data.setEffectiveDeskewAngle(params->deskewAngle());
			ui_data.setMode(params->mode());
		}
	}
	
	ImageTransformation new_xform(data.xform());
	new_xform.setCropArea(page_outline);
	
	if (!params.get()) {
		QRectF const image_area(
			new_xform.transformBack().mapRect(new_xform.resultingRect())
		);
		QRect const bounded_image_area(
			image_area.toRect().intersected(data.image().rect())
		);
		
		status.throwIfCancelled();
		
		if (bounded_image_area.isValid()) {
			BinaryImage rotated_image(
				orthogonalRotation(
					BinaryImage(
						data.image(), bounded_image_area,
						data.bwThreshold()
					),
					data.xform().preRotation().toDegrees()
				)
			);
			if (m_ptrDbg.get()) {
				m_ptrDbg->add(rotated_image, "bw_rotated");
			}
			
			QSize const unrotated_dpm(Dpm(data.image()).toSize());
			Dpm const rotated_dpm(
				data.xform().preRotation().rotate(unrotated_dpm)
			);
			cleanup(status, rotated_image, Dpi(rotated_dpm));
			if (m_ptrDbg.get()) {
				m_ptrDbg->add(rotated_image, "after_cleanup");
			}
			
			status.throwIfCancelled();
			
			SkewFinder skew_finder;
			skew_finder.setResolutionRatio(
				(double)rotated_dpm.horizontal() / rotated_dpm.vertical()
			);
			Skew const skew(skew_finder.findSkew(rotated_image));
			
			if (skew.confidence() >= skew.GOOD_CONFIDENCE) {
				ui_data.setEffectiveDeskewAngle(-skew.angle());
			} else {
				ui_data.setEffectiveDeskewAngle(0);
			}
			ui_data.setMode(MODE_AUTO);
			
			Params const new_params(
				ui_data.effectiveDeskewAngle(), deps, ui_data.mode()
			);
			m_ptrSettings->setPageParams(m_pageId, new_params);
			
			status.throwIfCancelled();
		}
	}
	
	new_xform.setPostRotation(ui_data.effectiveDeskewAngle());
	
	if (m_ptrNextTask) {
		return m_ptrNextTask->process(status, FilterData(data, new_xform));
	} else {
		return FilterResultPtr(
			new UiUpdater(
				m_ptrFilter, m_ptrDbg, data.image(),
				m_pageId, new_xform, ui_data
			)
		);
	}
}

void
Task::cleanup(TaskStatus const& status, BinaryImage& image, Dpi const& dpi)
{
	// We don't have to clean up every piece of garbage.
	// The only concern are the horizontal shadows, which we remove here.
	
	Dpi reduced_dpi(dpi);
	BinaryImage reduced_image;
	
	{
		ReduceThreshold reductor(image);
		while (reduced_dpi.horizontal() >= 200 && reduced_dpi.vertical() >= 200) {
			reductor.reduce(2);
			reduced_dpi = Dpi(
				reduced_dpi.horizontal() / 2,
				reduced_dpi.vertical() / 2
			);
		}
		reduced_image = reductor.image();
	}
	
	status.throwIfCancelled();
	
	QSize const brick(from150dpi(QSize(200, 14), reduced_dpi));
	BinaryImage opened(openBrick(reduced_image, brick, BLACK));
	reduced_image.release();
	
	status.throwIfCancelled();
	
	BinaryImage seed(expandPower2(opened, image.size(), WHITE));
	opened.release();
	
	status.throwIfCancelled();
	
	BinaryImage garbage(seedFill(seed, image, CONN8));
	seed.release();
	
	status.throwIfCancelled();
	
	rasterOp<RopSubtract<RopDst, RopSrc> >(image, garbage);
}

int
Task::from150dpi(int size, int target_dpi)
{
	int const new_size = (size * target_dpi + 75) / 150;
	if (new_size < 1) {
		return 1;
	}
	return new_size;
}

QSize
Task::from150dpi(QSize const& size, Dpi const& target_dpi)
{
	int const width = from150dpi(size.width(), target_dpi.horizontal());
	int const height = from150dpi(size.height(), target_dpi.vertical());
	return QSize(width, height);
}


/*============================ Task::UiUpdater ==========================*/

Task::UiUpdater::UiUpdater(
	IntrusivePtr<Filter> const& filter,
	std::auto_ptr<DebugImages> dbg_img,
	QImage const& image, LogicalPageId const& page_id,
	ImageTransformation const& xform,
	OptionsWidget::UiData const& ui_data)
:	m_ptrFilter(filter),
	m_ptrDbg(dbg_img),
	m_image(image),
	m_pageId(page_id),
	m_xform(xform),
	m_uiData(ui_data)
{
}

void
Task::UiUpdater::updateUI(FilterUiInterface* ui)
{
	// This function is executed from the GUI thread.
	
	OptionsWidget* const opt_widget = m_ptrFilter->optionsWidget();	
	opt_widget->postUpdateUI(m_uiData);
	ui->setOptionsWidget(opt_widget);
	
	ImageView* view = new ImageView(m_image, m_xform);
	ui->setImageWidget(view, m_ptrDbg.get());
	
	ui->invalidateThumbnail(m_pageId);
	
	QObject::connect(
		view, SIGNAL(manualDeskewAngleSet(double)),
		opt_widget, SLOT(manualDeskewAngleSetExternally(double))
	);
	QObject::connect(
		opt_widget, SIGNAL(manualDeskewAngleSet(double)),
		view, SLOT(manualDeskewAngleSetExternally(double))
	);
}

} // namespace deskew
