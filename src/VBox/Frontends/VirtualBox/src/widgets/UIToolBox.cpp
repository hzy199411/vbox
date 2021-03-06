/* $Id$ */
/** @file
 * VBox Qt GUI - UIToolBox class implementation.
 */

/*
 * Copyright (C) 2006-2020 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */

/* Qt includes: */
#include <QCheckBox>
#include <QLabel>
#include <QStyle>
#include <QVBoxLayout>

/* GUI includes: */
#include "QIRichTextLabel.h"
#include "QIToolButton.h"
#include "UICommon.h"
#include "UIIconPool.h"
#include "UIToolBox.h"
#include "UIWizardNewVM.h"

class UITest : public QWidget
{
    Q_OBJECT;
public:
    UITest(QWidget *pParent = 0)
        :QWidget(pParent)
    {}
    ~UITest(){}
};


/*********************************************************************************************************************************
*   UIToolBoxPage definition.                                                                                    *
*********************************************************************************************************************************/

class UIToolBoxPage : public QWidget
{

    Q_OBJECT;

signals:

    void sigShowPageWidget();

public:

    UIToolBoxPage(bool fEnableCheckBoxEnabled = false, QWidget *pParent = 0);
    void setTitle(const QString &strTitle);
    /* @p pWidget's ownership is transferred to the page. */
    void setWidget(QWidget *pWidget);
    void setTitleBackgroundColor(const QColor &color);
    void setPageWidgetVisible(bool fVisible);
    int index() const;
    void setIndex(int iIndex);
    int totalHeight() const;
    int titleHeight() const;
    int pageWidgetHeight() const;

protected:

    virtual bool eventFilter(QObject *pWatched, QEvent *pEvent) /* override */;

private slots:

    void sltHandleEnableToggle(int iState);

private:

    void prepare(bool fEnableCheckBoxEnabled);

    QVBoxLayout *m_pLayout;
    QWidget     *m_pTitleContainerWidget;
    QLabel      *m_pTitleLabel;
    QCheckBox   *m_pEnableCheckBox;

    QWidget     *m_pWidget;
    int          m_iIndex;
};

/*********************************************************************************************************************************
*   UIToolBoxPage implementation.                                                                                    *
*********************************************************************************************************************************/

UIToolBoxPage::UIToolBoxPage(bool fEnableCheckBoxEnabled /* = false */, QWidget *pParent /* = 0 */)
    :QWidget(pParent)
    , m_pLayout(0)
    , m_pTitleContainerWidget(0)
    , m_pTitleLabel(0)
    , m_pEnableCheckBox(0)
    , m_pWidget(0)
    , m_iIndex(0)
{
    prepare(fEnableCheckBoxEnabled);
}

void UIToolBoxPage::setTitle(const QString &strTitle)
{
    if (!m_pTitleLabel)
        return;
    m_pTitleLabel->setText(strTitle);
}

void UIToolBoxPage::prepare(bool fEnableCheckBoxEnabled)
{
    m_pLayout = new QVBoxLayout(this);
    m_pLayout->setContentsMargins(0, 0, 0, 0);

    m_pTitleContainerWidget = new QWidget;
    QHBoxLayout *pTitleLayout = new QHBoxLayout(m_pTitleContainerWidget);
    pTitleLayout->setContentsMargins(qApp->style()->pixelMetric(QStyle::PM_LayoutLeftMargin),
                                     .4f * qApp->style()->pixelMetric(QStyle::PM_LayoutTopMargin),
                                     qApp->style()->pixelMetric(QStyle::PM_LayoutRightMargin),
                                     .4f * qApp->style()->pixelMetric(QStyle::PM_LayoutBottomMargin));
    if (fEnableCheckBoxEnabled)
    {
        m_pEnableCheckBox = new QCheckBox;
        pTitleLayout->addWidget(m_pEnableCheckBox);
        connect(m_pEnableCheckBox, &QCheckBox::stateChanged, this, &UIToolBoxPage::sltHandleEnableToggle);
    }

    m_pTitleLabel = new QLabel;
    m_pTitleLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);

    m_pTitleLabel->installEventFilter(this);
    pTitleLayout->addWidget(m_pTitleLabel);
    m_pTitleContainerWidget->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    m_pLayout->addWidget(m_pTitleContainerWidget);
}

void UIToolBoxPage::setWidget(QWidget *pWidget)
{
    if (!m_pLayout || !pWidget)
        return;
    m_pWidget = pWidget;
    m_pLayout->addWidget(m_pWidget);

    if (m_pEnableCheckBox)
        m_pWidget->setEnabled(m_pEnableCheckBox->checkState() == Qt::Checked);

    m_pWidget->hide();
}

void UIToolBoxPage::setTitleBackgroundColor(const QColor &color)
{
    if (!m_pTitleLabel)
        return;
    QPalette palette = m_pTitleContainerWidget->palette();
    palette.setColor(QPalette::Window, color);
    m_pTitleContainerWidget->setPalette(palette);
    m_pTitleContainerWidget->setAutoFillBackground(true);
}

void UIToolBoxPage::setPageWidgetVisible(bool fVisible)
{
    if (m_pWidget)
        m_pWidget->setVisible(fVisible);
}

int UIToolBoxPage::index() const
{
    return m_iIndex;
}

void UIToolBoxPage::setIndex(int iIndex)
{
    m_iIndex = iIndex;
}

int UIToolBoxPage::totalHeight() const
{
    return pageWidgetHeight() + titleHeight();
}

int UIToolBoxPage::titleHeight() const
{
    if (m_pTitleContainerWidget && m_pTitleContainerWidget->sizeHint().isValid())
        return m_pTitleContainerWidget->sizeHint().height();
    return 0;
}

int UIToolBoxPage::pageWidgetHeight() const
{
    if (m_pWidget && m_pWidget->isVisible() && m_pWidget->sizeHint().isValid())
        return m_pWidget->sizeHint().height();
    return 0;
}

bool UIToolBoxPage::eventFilter(QObject *pWatched, QEvent *pEvent)
{
    if (pWatched == m_pTitleLabel && pEvent->type() == QEvent::MouseButtonPress)
        emit sigShowPageWidget();
    return QWidget::eventFilter(pWatched, pEvent);

}

void UIToolBoxPage::sltHandleEnableToggle(int iState)
{
    if (m_pWidget)
        m_pWidget->setEnabled(iState == Qt::Checked);
}


/*********************************************************************************************************************************
*   UIToolBox implementation.                                                                                    *
*********************************************************************************************************************************/

UIToolBox::UIToolBox(QWidget *pParent /*  = 0 */)
    : QIWithRetranslateUI<QFrame>(pParent)
    , m_iCurrentPageIndex(-1)
    , m_iPageCount(0)
{
    prepare();
    //setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);
}

bool UIToolBox::insertItem(int iIndex, QWidget *pWidget, const QString &strTitle, bool fAddEnableCheckBox /* = false */)
{
    if (m_pages.contains(iIndex))
        return false;
    ++m_iPageCount;
    UIToolBoxPage *pNewPage = new UIToolBoxPage(fAddEnableCheckBox, 0);;

    pNewPage->setWidget(pWidget);
    pNewPage->setIndex(iIndex);
    pNewPage->setTitle(strTitle);

    const QPalette pal = palette();
    QColor tabBackgroundColor = pal.color(QPalette::Active, QPalette::Highlight).lighter(110);
    pNewPage->setTitleBackgroundColor(tabBackgroundColor);

    m_pages[iIndex] = pNewPage;
    m_pMainLayout->insertWidget(iIndex, pNewPage);

    connect(pNewPage, &UIToolBoxPage::sigShowPageWidget,
            this, &UIToolBox::sltHandleShowPageWidget);

    static int iMaxPageHeight = 0;
    int iTotalTitleHeight = 0;
    foreach(UIToolBoxPage *pPage, m_pages)
    {
        if (pWidget && pWidget->sizeHint().isValid())
            iMaxPageHeight = qMax(iMaxPageHeight, pWidget->sizeHint().height());
        iTotalTitleHeight += pPage->titleHeight();
    }
    setMinimumHeight(m_iPageCount * (qApp->style()->pixelMetric(QStyle::PM_LayoutTopMargin) +
                                     qApp->style()->pixelMetric(QStyle::PM_LayoutBottomMargin)) +
                     iTotalTitleHeight +
                     iMaxPageHeight);

    return iIndex;
}

void UIToolBox::setItemEnabled(int iIndex, bool fEnabled)
{
    Q_UNUSED(fEnabled);
    Q_UNUSED(iIndex);
}

void UIToolBox::setItemText(int iIndex, const QString &strTitle)
{
    QMap<int, UIToolBoxPage*>::iterator iterator = m_pages.find(iIndex);
    if (iterator == m_pages.end())
        return;
    iterator.value()->setTitle(strTitle);
}

void UIToolBox::setItemIcon(int iIndex, const QIcon &icon)
{
    Q_UNUSED(iIndex);
    Q_UNUSED(icon);
}

void UIToolBox::setCurrentPage(int iIndex)
{
    m_iCurrentPageIndex = iIndex;
    QMap<int, UIToolBoxPage*>::iterator iterator = m_pages.find(iIndex);
    if (iterator == m_pages.end())
        return;
    foreach(UIToolBoxPage *pPage, m_pages)
        pPage->setPageWidgetVisible(false);

    iterator.value()->setPageWidgetVisible(true);
}

void UIToolBox::retranslateUi()
{
}

void UIToolBox::prepare()
{
    m_pMainLayout = new QVBoxLayout(this);
    m_pMainLayout->addStretch();
    //m_pMainLayout->setContentsMargins(0, 0, 0, 0);

    retranslateUi();
}

void UIToolBox::sltHandleShowPageWidget()
{
    UIToolBoxPage *pPage = qobject_cast<UIToolBoxPage*>(sender());
    if (!pPage)
        return;
    setCurrentPage(pPage->index());
    update();
}

#include "UIToolBox.moc"
