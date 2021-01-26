/* $Id$ */
/** @file
 * VBox Qt GUI - UIWizardNewVMPageBasic5 class implementation.
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
#include <QGridLayout>
#include <QMetaType>
#include <QRadioButton>
#include <QVBoxLayout>

/* GUI includes: */
#include "QIRichTextLabel.h"
#include "QIToolButton.h"
#include "UIIconPool.h"
#include "UIMediaComboBox.h"
#include "UIMedium.h"
#include "UIMediumSelector.h"
#include "UIMessageCenter.h"
#include "UIWizardNewVD.h"
#include "UIWizardNewVM.h"
#include "UIWizardNewVMPageBasic5.h"

UIWizardNewVMPage5::UIWizardNewVMPage5()
    : m_fRecommendedNoDisk(false)
    , m_pDiskSkip(0)
    , m_pDiskCreate(0)
    , m_pDiskPresent(0)
    , m_pDiskSelector(0)
    , m_pVMMButton(0)
{
}

void UIWizardNewVMPage5::updateVirtualDiskSource()
{
    if (!m_pDiskSelector || !m_pVMMButton)
        return;

    /* Enable/disable controls: */
    m_pDiskSelector->setEnabled(m_pDiskPresent->isChecked());
    m_pVMMButton->setEnabled(m_pDiskPresent->isChecked());

    /* Fetch filed values: */
    if (m_pDiskSkip->isChecked())
    {
        m_uVirtualDiskId = QUuid();
        m_strVirtualDiskName = QString();
        m_strVirtualDiskLocation = QString();
    }
    else if (m_pDiskPresent->isChecked())
    {
        m_uVirtualDiskId = m_pDiskSelector->id();
        m_strVirtualDiskName = m_pDiskSelector->currentText();
        m_strVirtualDiskLocation = m_pDiskSelector->location();
    }
}

void UIWizardNewVMPage5::getWithFileOpenDialog()
{
    /* Get opened medium id: */
    QUuid uMediumId;

    int returnCode = uiCommon().openMediumSelectorDialog(thisImp(), UIMediumDeviceType_HardDisk,
                                                           uMediumId,
                                                           fieldImp("machineFolder").toString(),
                                                           fieldImp("machineBaseName").toString(),
                                                           fieldImp("type").value<CGuestOSType>().GetId(),
                                                           false /* don't show/enable the create action: */);

    if (returnCode == static_cast<int>(UIMediumSelector::ReturnCode_Accepted) && !uMediumId.isNull())
    {
        /* Update medium-combo if necessary: */
        m_pDiskSelector->setCurrentItem(uMediumId);
        /* Update hard disk source: */
        updateVirtualDiskSource();
        /* Focus on hard disk combo: */
        m_pDiskSelector->setFocus();
    }
}

bool UIWizardNewVMPage5::getWithNewVirtualDiskWizard()
{
    /* Create New Virtual Hard Drive wizard: */
    UISafePointerWizardNewVD pWizard = new UIWizardNewVD(thisImp(),
                                                         fieldImp("machineBaseName").toString(),
                                                         fieldImp("machineFolder").toString(),
                                                         fieldImp("type").value<CGuestOSType>().GetRecommendedHDD(),
                                                         wizardImp()->mode());
    pWizard->prepare();
    bool fResult = false;
    if (pWizard->exec() == QDialog::Accepted)
    {
        fResult = true;
        m_virtualDisk = pWizard->virtualDisk();
        m_pDiskSelector->setCurrentItem(m_virtualDisk.GetId());
        m_pDiskPresent->click();
    }
    if (pWizard)
        delete pWizard;
    return fResult;
}

void UIWizardNewVMPage5::ensureNewVirtualDiskDeleted()
{
    /* Make sure virtual-disk valid: */
    if (m_virtualDisk.isNull())
        return;

    /* Remember virtual-disk attributes: */
    QString strLocation = m_virtualDisk.GetLocation();
    /* Prepare delete storage progress: */
    CProgress progress = m_virtualDisk.DeleteStorage();
    if (m_virtualDisk.isOk())
    {
        /* Show delete storage progress: */
        msgCenter().showModalProgressDialog(progress, thisImp()->windowTitle(), ":/progress_media_delete_90px.png", thisImp());
        if (!progress.isOk() || progress.GetResultCode() != 0)
            msgCenter().cannotDeleteHardDiskStorage(progress, strLocation, thisImp());
    }
    else
        msgCenter().cannotDeleteHardDiskStorage(m_virtualDisk, strLocation, thisImp());

    /* Detach virtual-disk anyway: */
    m_virtualDisk.detach();
}

void UIWizardNewVMPage5::retranslateWidgets()
{
    if (m_pDiskSkip)
        m_pDiskSkip->setText(UIWizardNewVM::tr("&Do not add a virtual hard disk"));
    if (m_pDiskCreate)
        m_pDiskCreate->setText(UIWizardNewVM::tr("&Create a virtual hard disk now"));
    if (m_pDiskPresent)
        m_pDiskPresent->setText(UIWizardNewVM::tr("&Use an existing virtual hard disk file"));
    if (m_pVMMButton)
        m_pVMMButton->setToolTip(UIWizardNewVM::tr("Choose a virtual hard disk file..."));
}

QWidget *UIWizardNewVMPage5::createDiskWidgets()
{
    QWidget *pDiskContainer = new QWidget;
    QGridLayout *pDiskLayout = new QGridLayout(pDiskContainer);

    m_pDiskSkip = new QRadioButton;
    m_pDiskCreate = new QRadioButton;
    m_pDiskPresent = new QRadioButton;
    QStyleOptionButton options;
    options.initFrom(m_pDiskPresent);
    int iWidth = m_pDiskPresent->style()->pixelMetric(QStyle::PM_ExclusiveIndicatorWidth, &options, m_pDiskPresent);
    pDiskLayout->setColumnMinimumWidth(0, iWidth);
    m_pDiskSelector = new UIMediaComboBox;
    {
        m_pDiskSelector->setType(UIMediumDeviceType_HardDisk);
        m_pDiskSelector->repopulate();
    }
    m_pVMMButton = new QIToolButton;
    {
        m_pVMMButton->setAutoRaise(true);
        m_pVMMButton->setIcon(UIIconPool::iconSet(":/select_file_16px.png", ":/select_file_disabled_16px.png"));
    }
    pDiskLayout->addWidget(m_pDiskSkip, 0, 0, 1, 3);
    pDiskLayout->addWidget(m_pDiskCreate, 1, 0, 1, 3);
    pDiskLayout->addWidget(m_pDiskPresent, 2, 0, 1, 3);
    pDiskLayout->addWidget(m_pDiskSelector, 3, 1);
    pDiskLayout->addWidget(m_pVMMButton, 3, 2);
    return pDiskContainer;
}

UIWizardNewVMPageBasic5::UIWizardNewVMPageBasic5()
    : m_pLabel(0)
{
    prepare();
    qRegisterMetaType<CMedium>();
    registerField("virtualDisk", this, "virtualDisk");
    registerField("virtualDiskId", this, "virtualDiskId");
    registerField("virtualDiskName", this, "virtualDiskName");
    registerField("virtualDiskLocation", this, "virtualDiskLocation");
}

void UIWizardNewVMPageBasic5::prepare()
{
    QVBoxLayout *pMainLayout = new QVBoxLayout(this);

    m_pLabel = new QIRichTextLabel(this);
    pMainLayout->addWidget(m_pLabel);
    pMainLayout->addWidget(createDiskWidgets());

    pMainLayout->addStretch();
    updateVirtualDiskSource();
    createConnections();
}

void UIWizardNewVMPageBasic5::createConnections()
{
    connect(m_pDiskSkip, &QRadioButton::toggled,
            this, &UIWizardNewVMPageBasic5::sltVirtualDiskSourceChanged);
    connect(m_pDiskCreate, &QRadioButton::toggled,
            this, &UIWizardNewVMPageBasic5::sltVirtualDiskSourceChanged);
    connect(m_pDiskPresent, &QRadioButton::toggled,
            this, &UIWizardNewVMPageBasic5::sltVirtualDiskSourceChanged);
    connect(m_pDiskSelector, static_cast<void(UIMediaComboBox::*)(int)>(&UIMediaComboBox::currentIndexChanged),
            this, &UIWizardNewVMPageBasic5::sltVirtualDiskSourceChanged);
    connect(m_pVMMButton, &QIToolButton::clicked,
            this, &UIWizardNewVMPageBasic5::sltGetWithFileOpenDialog);
}

void UIWizardNewVMPageBasic5::sltVirtualDiskSourceChanged()
{
    /* Call to base-class: */
    updateVirtualDiskSource();

    /* Broadcast complete-change: */
    emit completeChanged();
}

void UIWizardNewVMPageBasic5::sltGetWithFileOpenDialog()
{
    /* Call to base-class: */
    getWithFileOpenDialog();
}

void UIWizardNewVMPageBasic5::retranslateUi()
{
    setTitle(UIWizardNewVM::tr("Hard disk and Hardware"));

    QString strRecommendedHDD = field("type").value<CGuestOSType>().isNull() ? QString() :
                                UICommon::formatSize(field("type").value<CGuestOSType>().GetRecommendedHDD());
    if (m_pLabel)
        m_pLabel->setText(UIWizardNewVM::tr("<p>If you wish you can add a virtual hard disk to the new machine. "
                                            "You can either create a new hard disk file or select one from the list "
                                            "or from another location using the folder icon. "
                                            "If you need a more complex storage set-up you can skip this step "
                                            "and make the changes to the machine settings once the machine is created. "
                                            "The recommended size of the hard disk is <b>%1</b>.</p>")
                          .arg(strRecommendedHDD));

    retranslateWidgets();
}

void UIWizardNewVMPageBasic5::initializePage()
{
    retranslateUi();

    if (!field("type").canConvert<CGuestOSType>())
        return;

    CGuestOSType type = field("type").value<CGuestOSType>();

    if (type.GetRecommendedHDD() != 0)
    {
        if (m_pDiskCreate)
        {
            m_pDiskCreate->setFocus();
            m_pDiskCreate->setChecked(true);
        }
        m_fRecommendedNoDisk = false;
    }
    else
    {
        if (m_pDiskSkip)
        {
            m_pDiskSkip->setFocus();
            m_pDiskSkip->setChecked(true);
        }
        m_fRecommendedNoDisk = true;
    }
    if (m_pDiskSelector)
        m_pDiskSelector->setCurrentIndex(0);
}

void UIWizardNewVMPageBasic5::cleanupPage()
{
    /* Call to base-class: */
    ensureNewVirtualDiskDeleted();
    UIWizardPage::cleanupPage();
}

bool UIWizardNewVMPageBasic5::isComplete() const
{
    if (!m_pDiskSkip)
        return false;
    return m_pDiskSkip->isChecked() ||
        !m_pDiskPresent->isChecked() ||
        !uiCommon().medium(m_pDiskSelector->id()).isNull();
}

bool UIWizardNewVMPageBasic5::validatePage()
{
    /* Initial result: */
    bool fResult = true;

    /* Ensure unused virtual-disk is deleted: */
    if (m_pDiskSkip->isChecked() || m_pDiskCreate->isChecked() || (!m_virtualDisk.isNull() && m_uVirtualDiskId != m_virtualDisk.GetId()))
        ensureNewVirtualDiskDeleted();

    if (m_pDiskSkip->isChecked())
    {
        /* Ask user about disk-less machine unless that's the recommendation: */
        if (!m_fRecommendedNoDisk)
            fResult = msgCenter().confirmHardDisklessMachine(thisImp());
    }
    else if (m_pDiskCreate->isChecked())
    {
        /* Show the New Virtual Hard Drive wizard: */
        fResult = getWithNewVirtualDiskWizard();
    }

    if (fResult)
    {
        /* Lock finish button: */
        startProcessing();

        /* Try to create VM: */
        fResult = qobject_cast<UIWizardNewVM*>(wizard())->createVM();

        /* Unlock finish button: */
        endProcessing();
    }

    /* Return result: */
    return fResult;
}
