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

#ifndef DESKEW_FILTER_H_
#define DESKEW_FILTER_H_

#include "NonCopyable.h"
#include "AbstractFilter.h"
#include "IntrusivePtr.h"
#include "FilterResult.h"

class PageId;
class LogicalPageId;
class PageLayout;
class QString;

namespace select_content
{
	class Task;
	class ThumbnailTask;
}

namespace deskew
{

class OptionsWidget;
class Task;
class ThumbnailTask;
class Settings;

class Filter : public AbstractFilter
{
	DECLARE_NON_COPYABLE(Filter)
public:
	Filter();
	
	virtual ~Filter();
	
	virtual QString getName() const;
	
	virtual PageSequence::View getView() const;
	
	virtual void preUpdateUI(FilterUiInterface* ui, PageId const& page_id);
	
	virtual QDomElement saveSettings(
		ProjectWriter const& writer, QDomDocument& doc) const;
	
	virtual void loadSettings(
		ProjectReader const& reader, QDomElement const& filters_el);
	
	IntrusivePtr<Task> createTask(
		PageId const& page_id,
		IntrusivePtr<select_content::Task> const& next_task, bool debug);
	
	IntrusivePtr<ThumbnailTask> createThumbnailTask(
		IntrusivePtr<select_content::ThumbnailTask> const& next_task);
	
	OptionsWidget* optionsWidget() { return m_ptrOptionsWidget.get(); }
private:
	void writePageSettings(
		QDomDocument& doc, QDomElement& filter_el,
		LogicalPageId const& page_id, int numeric_id) const;
	
	IntrusivePtr<Settings> m_ptrSettings;
	std::auto_ptr<OptionsWidget> m_ptrOptionsWidget;
};

} // namespace deskew

#endif