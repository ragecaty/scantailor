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

#include "Filter.h"
#include "FilterUiInterface.h"
#include "OptionsWidget.h"
#include "Task.h"
#include "Settings.h"
#include "PageSequence.h"
#include "ProjectReader.h"
#include "ProjectWriter.h"
#include "PageId.h"
#include "ImageId.h"
#include "Rule.h"
#include "Params.h"
#include "ThumbnailTask.h"
#include <boost/lambda/lambda.hpp>
#include <boost/lambda/bind.hpp>
#include <QString>
#include <QObject>
#include <QDomElement>
#include <stddef.h>

namespace page_split
{

Filter::Filter(IntrusivePtr<PageSequence> const& page_sequence)
:	m_ptrPages(page_sequence),
	m_ptrSettings(new Settings)
{
	m_ptrOptionsWidget.reset(new OptionsWidget(m_ptrSettings));
}

Filter::~Filter()
{
}

QString
Filter::getName() const
{
	return QObject::tr("Split pages");
}

PageSequence::View
Filter::getView() const
{
	return PageSequence::IMAGE_VIEW;
}

void
Filter::preUpdateUI(FilterUiInterface* ui, PageId const& page_id)
{
	m_ptrOptionsWidget->preUpdateUI(page_id);
	ui->setOptionsWidget(m_ptrOptionsWidget.get());
}

QDomElement
Filter::saveSettings(
	ProjectWriter const& writer, QDomDocument& doc) const
{
	using namespace boost::lambda;
	
	QDomElement filter_el(doc.createElement("page-split"));
	filter_el.setAttribute(
		"defaultLayoutType",
		Rule::layoutTypeToString(m_ptrSettings->defaultLayoutType())
	);
	
	writer.enumImages(
		bind(
			&Filter::writeImageSettings,
			this, boost::ref(doc), var(filter_el), _1, _2
		)
	);
	
	return filter_el;
}

void
Filter::loadSettings(ProjectReader const& reader, QDomElement const& filters_el)
{
	m_ptrSettings->clear();
	
	QDomElement const filter_el(filters_el.namedItem("page-split").toElement());
	QString const default_layout_type(filter_el.attribute("defaultLayoutType"));
	m_ptrSettings->applyToAllPages(Rule::layoutTypeFromString(default_layout_type));
	
	QString const image_tag_name("image");
	QDomNode node(filter_el.firstChild());
	for (; !node.isNull(); node = node.nextSibling()) {
		if (!node.isElement()) {
			continue;
		}
		if (node.nodeName() != image_tag_name) {
			continue;
		}
		QDomElement el(node.toElement());
		
		bool ok = true;
		int const id = el.attribute("id").toInt(&ok);
		if (!ok) {
			continue;
		}
		
		ImageId const image_id(reader.imageId(id));
		if (image_id.isNull()) {
			continue;
		}
		
		QString const layout_type(el.attribute("layoutType"));
		if (!layout_type.isEmpty()) {
			m_ptrSettings->applyToPage(
				image_id, Rule::layoutTypeFromString(layout_type)
			);
		}
		
		QDomElement params_el(el.namedItem("params").toElement());
		if (!params_el.isNull()) {
			m_ptrSettings->setPageParams(image_id, Params(params_el));
		}
	}
}

void
Filter::writeImageSettings(
	QDomDocument& doc, QDomElement& filter_el,
	ImageId const& image_id, int const numeric_id) const
{
	Rule const rule(m_ptrSettings->getRuleFor(image_id));
	
	QDomElement image_el(doc.createElement("image"));
	image_el.setAttribute("id", numeric_id);
	if (rule.scope() != Rule::ALL_PAGES) {
		// If scope is ALL_PAGES, this layout type is the default one.
		// We've already written it as "defaultLayoutType".
		image_el.setAttribute("layoutType", rule.layoutTypeAsString());
	}
	
	std::auto_ptr<Params> const params(m_ptrSettings->getPageParams(image_id));
	if (!params.get() && rule.layoutType() == Rule::AUTO_DETECT) {
		return;
	}
	
	image_el.appendChild(params->toXml(doc, "params"));
	filter_el.appendChild(image_el);
}

IntrusivePtr<Task>
Filter::createTask(
	PageId const& page_id,
	IntrusivePtr<deskew::Task> const& next_task, bool debug)
{
	return IntrusivePtr<Task>(
		new Task(
			IntrusivePtr<Filter>(this),
			m_ptrSettings, m_ptrPages, next_task, page_id, debug
		)
	);
}

IntrusivePtr<ThumbnailTask>
Filter::createThumbnailTask(
	IntrusivePtr<deskew::ThumbnailTask> const& next_task)
{
	return IntrusivePtr<ThumbnailTask>(
		new ThumbnailTask(m_ptrSettings, next_task)
	);
}

} // page_split