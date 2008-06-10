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

#include "MainWindow.h.moc"
#include "WorkerThread.h"
#include "PageSequence.h"
#include "ThumbnailSequence.h"
#include "PageInfo.h"
#include "ImageId.h"
#include "FilterOptionsWidget.h"
#include "DebugImages.h"
#include "BasicImageView.h"
#include "ProjectWriter.h"
#include "ProjectReader.h"
#include "ThumbnailPixmapCache.h"
#include "ThumbnailFactory.h"
#include "filters/fix_orientation/Filter.h"
#include "filters/fix_orientation/Task.h"
#include "filters/fix_orientation/ThumbnailTask.h"
#include "filters/page_split/Filter.h"
#include "filters/page_split/Task.h"
#include "filters/page_split/ThumbnailTask.h"
#include "filters/deskew/Filter.h"
#include "filters/deskew/Task.h"
#include "filters/deskew/ThumbnailTask.h"
#include "filters/select_content/Filter.h"
#include "filters/select_content/Task.h"
#include "filters/select_content/ThumbnailTask.h"
#include "LoadFileTask.h"
#include "ScopedIncDec.h"
#include <QLineF>
#include <QStackedLayout>
#include <QLayoutItem>
#include <QAbstractListModel>
#include <QFileInfo>
#include <QString>
#include <QVariant>
#include <QModelIndex>
#include <QFileDialog>
#include <QMessageBox>
#include <QGraphicsItem>
#include <QDebug>
#include <algorithm>
#include <stddef.h>
#include <assert.h>

class MainWindow::FilterListModel : public QAbstractListModel
{
	DECLARE_NON_COPYABLE(FilterListModel)
public:
	FilterListModel(IntrusivePtr<PageSequence> const& page_sequence);
	
	virtual ~FilterListModel();
	
	std::vector<FilterPtr> const& filters() const { return m_filters; }
	
	FilterPtr const& getFilter(int filter_idx) const;
	
	int getFilterIndex(FilterPtr const& filter) const;
	
	BackgroundTaskPtr createCompositeTask(
		PageInfo const& page, int last_filter_idx,
		ThumbnailPixmapCache& thumbnail_cache,
		bool batch_processing, bool debug);
	
	IntrusivePtr<fix_orientation::ThumbnailTask>
	createCompositeThumbnailTask(int last_filter_idx);
	
	virtual int rowCount(QModelIndex const& parent) const;
	
	virtual QVariant data(QModelIndex const& index, int role) const;
private:
	IntrusivePtr<fix_orientation::Filter> m_ptrFixOrientationFilter;
	IntrusivePtr<page_split::Filter> m_ptrPageSplitFilter;
	IntrusivePtr<deskew::Filter> m_ptrDeskewFilter;
	IntrusivePtr<select_content::Filter> m_ptrSelectContentFilter;
	std::vector<FilterPtr> m_filters;
};


MainWindow::MainWindow(
	std::vector<ImageFileInfo> const& files, QString const& out_dir)
:	m_ptrPages(new PageSequence(files, PageSequence::AUTO_PAGES)),
	m_frozenPages(PageSequence::IMAGE_VIEW),
	m_outDir(out_dir),
	m_ptrThumbnailCache(createThumbnailCache()),
	m_ptrThumbSequence(new ThumbnailSequence),
	m_curFilter(0),
	m_ignoreSelectionChanges(0),
	m_ignorePageChanges(0),
	m_workerThreadReady(false),
	m_debug(false),
	m_projectModified(true)
{
	m_ptrFilterListModel.reset(new FilterListModel(m_ptrPages));
	
	construct();
}

MainWindow::MainWindow(
	QString const& project_file, ProjectReader const& project_reader)
:	m_ptrPages(project_reader.pages()),
	m_frozenPages(PageSequence::IMAGE_VIEW),
	m_outDir(project_reader.outputDirectory()),
	m_projectFile(project_file),
	m_ptrThumbnailCache(createThumbnailCache()),
	m_ptrThumbSequence(new ThumbnailSequence),
	m_curFilter(0),
	m_ignoreSelectionChanges(0),
	m_ignorePageChanges(0),
	m_workerThreadReady(false),
	m_debug(false),
	m_projectModified(false),
	m_batchProcessing(false)
{
	m_ptrFilterListModel.reset(new FilterListModel(m_ptrPages));
	project_reader.readFilterSettings(m_ptrFilterListModel->filters());
	
	construct();
}

MainWindow::~MainWindow()
{
	if (m_ptrCurTask) {
		m_ptrCurTask->cancel();
		m_ptrCurTask.reset();
	}
	m_pWorkerThread->exit();
	m_pWorkerThread->wait();
}

std::auto_ptr<ThumbnailPixmapCache>
MainWindow::createThumbnailCache()
{
	return std::auto_ptr<ThumbnailPixmapCache>(
		new ThumbnailPixmapCache(
			m_outDir+"/cache/thumbs", QSize(200, 200), 40, 5
		)
	);
}

void
MainWindow::construct()
{
	setupUi(this);
	
	m_ptrThumbSequence->setThumbnailFactory(
		IntrusivePtr<ThumbnailFactory>(
			new ThumbnailFactory(
				*m_ptrThumbnailCache,
				m_ptrFilterListModel->createCompositeThumbnailTask(m_curFilter)
			)
		)
	);
	m_ptrThumbSequence->attachView(thumbView);
	
	addAction(actionNextPageOnPgDown);
	addAction(actionPrevPageOnPgUp);
	
	m_debug = actionDebug->isChecked();
	m_pImageFrameLayout = new QStackedLayout(imageViewFrame);
	m_pOptionsFrameLayout = new QStackedLayout(filterOptions);
	
	filterList->setModel(m_ptrFilterListModel.get());
	filterList->selectionModel()->select(
		m_ptrFilterListModel->index(0, 0),
		QItemSelectionModel::SelectCurrent
	);
	
	syncPageSequence();
	m_ptrThumbSequence->reset(m_frozenPages);
	
	connect(
		filterList->selectionModel(),
		SIGNAL(selectionChanged(QItemSelection const&, QItemSelection const&)),
		this, SLOT(filterSelectionChanged(QItemSelection const&))
	);
	
	m_pWorkerThread = new WorkerThread(this);
	connect(m_pWorkerThread, SIGNAL(ready()), this, SLOT(workerThreadReady()));
	connect(
		m_pWorkerThread,
		SIGNAL(finished(BackgroundTaskPtr const&, FilterResultPtr const&)),
		this, SLOT(filterResult(BackgroundTaskPtr const&, FilterResultPtr const&))
	);
	connect(
		pageSpinBox, SIGNAL(valueChanged(int)),
		this, SLOT(pageChanged(int))
	);
	
	connect(
		actionDebug, SIGNAL(toggled(bool)),
		this, SLOT(debugToggled(bool))
	);
	
	connect(
		actionSaveProject, SIGNAL(triggered(bool)),
		this, SLOT(saveProjectTriggered())
	);
	connect(
		actionSaveProjectAs, SIGNAL(triggered(bool)),
		this, SLOT(saveProjectAsTriggered())
	);
	
	connect(
		m_ptrPages.get(), SIGNAL(modified()),
		this, SLOT(pageSequenceModified()), Qt::QueuedConnection
	);
	
	updateWindowTitle();
	
	m_pWorkerThread->start();
}

void
MainWindow::setOptionsWidget(FilterOptionsWidget* widget)
{
	if (m_ptrOptionsWidget == widget) {
		return;
	}
	
	if (m_ptrOptionsWidget) {
		disconnect(
			m_ptrOptionsWidget, SIGNAL(reloadRequested()),
			this, SLOT(reloadRequested())
		);
		disconnect(
			m_ptrOptionsWidget, SIGNAL(startBatchProcessing()),
			this, SLOT(startBatchProcessing())
		);
		disconnect(
			m_ptrOptionsWidget, SIGNAL(stopBatchProcessing()),
			this, SLOT(stopBatchProcessing())
		);
		disconnect(
			m_ptrOptionsWidget, SIGNAL(invalidateThumbnail(PageId const&)),
			this, SLOT(invalidateThumbnailSlot(PageId const&))
		);
	}
	
	m_pOptionsFrameLayout->removeWidget(m_ptrOptionsWidget);
	m_pOptionsFrameLayout->addWidget(widget);
	m_ptrOptionsWidget = widget;
	
	connect(widget, SIGNAL(reloadRequested()), this, SLOT(reloadRequested()));
	connect(widget, SIGNAL(startBatchProcessing()), this, SLOT(startBatchProcessing()));
	connect(widget, SIGNAL(stopBatchProcessing()), this, SLOT(stopBatchProcessing()));
	connect(
		widget, SIGNAL(invalidateThumbnail(PageId const&)),
		this, SLOT(invalidateThumbnailSlot(PageId const&))
	);
}

void
MainWindow::setImageWidget(
	QWidget* widget, DebugImages const* debug_images)
{
	removeWidgetsFromLayout(m_pImageFrameLayout, true);
	if (!debug_images || debug_images->items().empty()) {
		m_pImageFrameLayout->addWidget(widget);
	} else {
		QTabWidget* tab_widget = new QTabWidget;
		tab_widget->addTab(widget, "Main");
		std::list<DebugImages::Item> const& items = debug_images->items();
		std::list<DebugImages::Item>::const_iterator it(items.begin());
		std::list<DebugImages::Item>::const_iterator const end(items.end());
		for (; it != end; ++it) {
			tab_widget->addTab(new BasicImageView(it->image()), it->label());
		}
		m_pImageFrameLayout->addWidget(tab_widget);
	}
}

void
MainWindow::invalidateThumbnail(PageId const& page_id)
{
	m_ptrThumbSequence->invalidateThumbnail(page_id);
}

void
MainWindow::pageChanged(int const page)
{
	if (m_ignorePageChanges) {
		return;
	}
	
	unsigned const zero_based_page = page - 1;
	PageInfo page_info(m_frozenPages.pageAt(zero_based_page));
	
	if (zero_based_page == m_frozenPages.curPageIdx() - 1) {
		page_info = m_ptrPages->setPrevPage(m_frozenPages.view());
	} else if (zero_based_page == m_frozenPages.curPageIdx() + 1) {
		page_info = m_ptrPages->setNextPage(m_frozenPages.view());
	} else {
		m_ptrPages->setCurPage(page_info.id());
	}
	
	m_batchProcessing = false;
	loadImage(page_info);
}

void
MainWindow::filterSelectionChanged(QItemSelection const& selected)
{
	if (m_ignoreSelectionChanges) {
		return;
	}
	
	if (selected.empty()) {
		return;
	}
	
	m_curFilter = selected.front().top();
	syncPageSequence();
	
	m_ptrThumbSequence->setThumbnailFactory(
		IntrusivePtr<ThumbnailFactory>(
			new ThumbnailFactory(
				*m_ptrThumbnailCache,
				m_ptrFilterListModel->createCompositeThumbnailTask(m_curFilter)
			)
		)
	);
	m_ptrThumbSequence->reset(m_frozenPages);
	
	loadImage();
}

void
MainWindow::workerThreadReady()
{
	m_workerThreadReady = true;
	loadImage();
}

void
MainWindow::reloadRequested()
{
	loadImage();
}

void
MainWindow::startBatchProcessing()
{
	m_batchProcessing = true;
	
	{
		ScopedIncDec<int> guard(m_ignorePageChanges);
		pageSpinBox->setEnabled(false);
		pageSlider->setEnabled(false);
		pageSpinBox->setValue(1);
	}
	
	PageInfo const page_info(m_frozenPages.pageAt(0));
	m_ptrPages->setCurPage(page_info.id());
	loadImage(page_info);
}

void
MainWindow::stopBatchProcessing()
{
	pageSpinBox->setEnabled(true);
	pageSlider->setEnabled(true);
	if (m_ptrCurTask) {
		m_ptrCurTask->cancel();
		m_ptrCurTask.reset();
	}
	m_batchProcessing = false;
}

void
MainWindow::invalidateThumbnailSlot(PageId const& page_id)
{
	invalidateThumbnail(page_id);
}

void
MainWindow::filterResult(BackgroundTaskPtr const& task, FilterResultPtr const& result)
{
	if (task != m_ptrCurTask || task->isCancelled()) {
		// The page sequence may still have changed.
		syncPageSequence();
		return;
	}
	
	if (!result->filter()) {
		// Error loading file.
		m_batchProcessing = false;
	} else if (result->filter() != m_ptrFilterListModel->getFilter(m_curFilter)) {
		// Error from one of the previous filters.
		m_batchProcessing = false;
		
		int const idx = m_ptrFilterListModel->getFilterIndex(result->filter());
		assert(idx >= 0);
		m_curFilter = idx;
		
		ScopedIncDec<int> selection_guard(m_ignoreSelectionChanges);
		filterList->selectionModel()->select(
			m_ptrFilterListModel->index(idx, 0),
			QItemSelectionModel::SelectCurrent
		);
	}
	
	syncPageSequence(); // must be done after m_curFilter is modified.
	result->updateUI(this);
	
	if (m_batchProcessing) {
		if (m_frozenPages.curPageIdx() == m_frozenPages.numPages() - 1) {
			m_batchProcessing = false;
		} else {
			loadImage(m_ptrPages->setNextPage(m_frozenPages.view()));
		}
	}
}

void
MainWindow::debugToggled(bool const enabled)
{
	m_debug = enabled;
}

void
MainWindow::saveProjectTriggered()
{
	if (m_projectFile.isEmpty()) {
		saveProjectAsTriggered();
		return;
	}
	
	if (saveProjectWithFeedback(m_projectFile)) {
		m_projectModified = false;
		updateWindowTitle();
	}
}

void
MainWindow::saveProjectAsTriggered()
{
	QString project_file(
		QFileDialog::getSaveFileName(
			this, QString(), m_projectFile,
			tr("Scan Tailor Projects")+" (*.ScanTailor)"
		)
	);
	if (!project_file.endsWith(".ScanTailor", Qt::CaseInsensitive)) {
		project_file += ".ScanTailor";
	}
	
	if (saveProjectWithFeedback(project_file)) {
		m_projectFile = project_file;
		m_projectModified = false;
		updateWindowTitle();
	}
}

void
MainWindow::pageSequenceModified()
{
	m_projectModified = true;
	updateWindowTitle();
}

void
MainWindow::removeWidgetsFromLayout(QLayout* layout, bool delete_widgets)
{
	QLayoutItem *child;
	while ((child = layout->takeAt(0))) {
		if (delete_widgets) {
			delete child->widget();
		}
		delete child;
	}
}

void
MainWindow::loadImage()
{
	if (!m_workerThreadReady) {
		return;
	}
	
	loadImage(m_ptrPages->curPage(m_frozenPages.view()));
}

void
MainWindow::loadImage(PageInfo const& page)
{
	if (!m_workerThreadReady) {
		return;
	}
	
	removeWidgetsFromLayout(m_pImageFrameLayout, true);
	m_ptrFilterListModel->getFilter(m_curFilter)->preUpdateUI(this, page.id());
	
	if (m_ptrCurTask) {
		m_ptrCurTask->cancel();
		m_ptrCurTask.reset();
	}
	m_ptrCurTask = m_ptrFilterListModel->createCompositeTask(
		page, m_curFilter, *m_ptrThumbnailCache, m_batchProcessing, m_debug
	);
	if (m_ptrCurTask) {
		m_pWorkerThread->executeCommand(m_ptrCurTask);
	}
}

void
MainWindow::syncPageSequence()
{
	FilterPtr const& filter = m_ptrFilterListModel->getFilter(m_curFilter);
	m_frozenPages = m_ptrPages->snapshot(filter->getView());
	
	m_ptrThumbSequence->setThumbnailFactory(
		IntrusivePtr<ThumbnailFactory>(
			new ThumbnailFactory(
				*m_ptrThumbnailCache,
				m_ptrFilterListModel->createCompositeThumbnailTask(m_curFilter)
			)
		)
	);
	
	int const from = 1;
	int const to = m_frozenPages.numPages();
	
	ScopedIncDec<int> guard(m_ignorePageChanges);
	pageSlider->setRange(from, to);
	pageSpinBox->setRange(from, to);
	pageSpinBox->setSuffix(QString(" / %1").arg(to));
	pageSpinBox->setValue(m_frozenPages.curPageIdx() + 1);
}

void
MainWindow::updateWindowTitle()
{
	QString project_name;
	if (m_projectFile.isEmpty()) {
		project_name = tr("Unnamed");
	} else {
		project_name = QFileInfo(m_projectFile).baseName();
	}
	
	// We don't include "[modified]" if m_projectModified is true,
	// because very innocent things like moving to a next page will
	// in fact mark the project as modified.
	setWindowTitle(tr("%1 - Scan Tailor").arg(project_name));
}

bool
MainWindow::saveProjectWithFeedback(QString const& project_file)
{
	ProjectWriter writer(m_outDir, m_ptrPages);
	
	if (!writer.write(project_file, m_ptrFilterListModel->filters())) {
		QMessageBox::warning(
			this, tr("Error"),
			tr("Error saving the project file!")
		);
		return false;
	}
	
	return true;
}


/*====================== MainWindow::FilterListModel =====================*/

MainWindow::FilterListModel::FilterListModel(
	IntrusivePtr<PageSequence> const& page_sequence)
:	m_ptrFixOrientationFilter(new fix_orientation::Filter(page_sequence)),
	m_ptrPageSplitFilter(new page_split::Filter(page_sequence)),
	m_ptrDeskewFilter(new deskew::Filter()),
	m_ptrSelectContentFilter(new select_content::Filter())
{
	m_filters.push_back(m_ptrFixOrientationFilter);
	m_filters.push_back(m_ptrPageSplitFilter);
	m_filters.push_back(m_ptrDeskewFilter);
	m_filters.push_back(m_ptrSelectContentFilter);
}

MainWindow::FilterListModel::~FilterListModel()
{
}

MainWindow::FilterPtr const&
MainWindow::FilterListModel::getFilter(int const filter_idx) const
{
	return m_filters[filter_idx];
}

int
MainWindow::FilterListModel::getFilterIndex(FilterPtr const& filter) const
{
	size_t const num_filters = m_filters.size();
	for (size_t i = 0; i < num_filters; ++i) {
		if (m_filters[i] == filter) {
			return i;
		}
	}
	
	return -1;
}

BackgroundTaskPtr
MainWindow::FilterListModel::createCompositeTask(
	PageInfo const& page, int const last_filter_idx,
	ThumbnailPixmapCache& thumbnail_cache,
	bool const batch_processing, bool const debug)
{
	IntrusivePtr<fix_orientation::Task> fix_orientation_task;
	IntrusivePtr<page_split::Task> page_split_task;
	IntrusivePtr<deskew::Task> deskew_task;
	IntrusivePtr<select_content::Task> select_content_task;
	
	switch (last_filter_idx) {
	case 3:
		select_content_task = m_ptrSelectContentFilter->createTask(
			page.id(), batch_processing, debug
		);
	case 2:
		deskew_task = m_ptrDeskewFilter->createTask(
			page.id(), select_content_task, debug
		);
	case 1:
		page_split_task = m_ptrPageSplitFilter->createTask(
			page.id(), deskew_task, debug
		);
	case 0:
		fix_orientation_task = m_ptrFixOrientationFilter->createTask(
			page.id(), page_split_task
		);
	}
	assert(fix_orientation_task);
	
	return BackgroundTaskPtr(
		new LoadFileTask(page, thumbnail_cache, fix_orientation_task)
	);
}

IntrusivePtr<fix_orientation::ThumbnailTask>
MainWindow::FilterListModel::createCompositeThumbnailTask(int const last_filter_idx)
{
	IntrusivePtr<fix_orientation::ThumbnailTask> fix_orientation_task;
	IntrusivePtr<page_split::ThumbnailTask> page_split_task;
	IntrusivePtr<deskew::ThumbnailTask> deskew_task;
	IntrusivePtr<select_content::ThumbnailTask> select_content_task;
	
	switch (last_filter_idx) {
	case 3:
		select_content_task = m_ptrSelectContentFilter->createThumbnailTask();
	case 2:
		deskew_task = m_ptrDeskewFilter->createThumbnailTask(select_content_task);
	case 1:
		page_split_task = m_ptrPageSplitFilter->createThumbnailTask(deskew_task);
	case 0:
		fix_orientation_task = m_ptrFixOrientationFilter->createThumbnailTask(page_split_task);
	}
	assert(fix_orientation_task);
	
	return fix_orientation_task;
}

int
MainWindow::FilterListModel::rowCount(QModelIndex const& parent) const
{
	return m_filters.size();
}

QVariant
MainWindow::FilterListModel::data(QModelIndex const& index, int const role) const
{
	if (role == Qt::DisplayRole) {
		return m_filters[index.row()]->getName();
	}
	return QVariant();
}