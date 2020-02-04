/* $Id$ */
/** @file
 * VBox Qt GUI - UIMachinePreview class declaration.
 */

/*
 * Copyright (C) 2010-2020 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */

#ifndef FEQT_INCLUDED_SRC_manager_details_UIMachinePreview_h
#define FEQT_INCLUDED_SRC_manager_details_UIMachinePreview_h
#ifndef RT_WITHOUT_PRAGMA_ONCE
# pragma once
#endif

/* Qt includes: */
#include <QHash>

/* GUI includes: */
#include "QIWithRetranslateUI.h"
#include "UIDetailsItem.h"
#include "UIExtraDataDefs.h"

/* COM includes: */
#include "COMEnums.h"
#include "CMachine.h"
#include "CSession.h"

/* Forward declarations: */
class QAction;
class QImage;
class QPixmap;
class QMenu;
class QTimer;

/** QIGraphicsWidget sub-class used as VM Preview widget inside Details pane. */
class UIMachinePreview : public QIWithRetranslateUI4<QIGraphicsWidget>
{
    Q_OBJECT;

signals:

    /** @name Layout stuff.
      * @{ */
        /** Notifies about size-hint changes. */
        void sigSizeHintChanged();
    /** @} */

public:

    /** RTTI item type. */
    enum { Type = UIDetailsItemType_Preview };

    /** Constructs preview element, passing pParent to the base-class. */
    UIMachinePreview(QIGraphicsWidget *pParent);
    /** Destructs preview element. */
    virtual ~UIMachinePreview() /* override */;

    /** @name Item stuff.
      * @{ */
        /** Defines @a comMachine to make preview for. */
        void setMachine(const CMachine &comMachine);
        /** Retuirns machine we do preview for. */
        CMachine machine() const;
    /** @} */

protected:

    /** @name Event-handling stuff.
      * @{ */
        /** Handles translation event. */
        virtual void retranslateUi() /* override */;

        /** Handles resize @a pEvent. */
        virtual void resizeEvent(QGraphicsSceneResizeEvent *pEvent) /* override */;

        /** Handles show @a pEvent. */
        virtual void showEvent(QShowEvent *pEvent) /* override */;
        /** Handles hide @a pEvent. */
        virtual void hideEvent(QHideEvent *pEvent) /* override */;

        /** Handles context-menu @a pEvent. */
        virtual void contextMenuEvent(QGraphicsSceneContextMenuEvent *pEvent) /* override */;

        /** Performs painting using passed @a pPainter, @a pOptions and optionally specified @a pWidget. */
        virtual void paint(QPainter *pPainter, const QStyleOptionGraphicsItem *pOptions, QWidget *pWidget = 0) /* override */;
    /** @} */

    /** @name Item stuff.
      * @{ */
        /** Returns RTTI item type. */
        virtual int type() const /* override */ { return Type; }
    /** @} */

    /** @name Layout stuff.
      * @{ */
        /** Returns size-hint.
          * @param  enmWhich    Brings size-hint type.
          * @param  constraint  Brings size constraint. */
        virtual QSizeF sizeHint(Qt::SizeHint enmWhich, const QSizeF &constraint = QSizeF()) const /* override */;
    /** @} */

private slots:

    /** @name Event-handling stuff.
      * @{ */
        /** Handles machine-state change for item with @a uId. */
        void sltMachineStateChange(const QUuid &uId);
    /** @} */

    /** @name Item stuff.
      * @{ */
        /** Handles request to recreate preview. */
        void sltRecreatePreview();
    /** @} */

private:

    /** Aspect ratio presets. */
    enum AspectRatioPreset
    {
        AspectRatioPreset_16x10,
        AspectRatioPreset_16x9,
        AspectRatioPreset_4x3,
    };

    /** @name Prepare/cleanup cascade.
      * @{ */
        /** Prepares all. */
        void prepare();

        /** Cleanups all. */
        void cleanup();
    /** @} */

    /** @name Item stuff.
      * @{ */
        /** Define update @a enmInterval, @a fSave it if requested. */
        void setUpdateInterval(PreviewUpdateIntervalType enmInterval, bool fSave);

        /** Recalculates preview rectangle. */
        void recalculatePreviewRectangle();

        /** Restarts preview uppdate routine. */
        void restart();
        /** Stops preview uppdate routine. */
        void stop();
    /** @} */

    /** Looks for the best aspect-ratio preset for the passed @a dAspectRatio among all the passed @a ratios. */
    static AspectRatioPreset bestAspectRatioPreset(const double dAspectRatio, const QMap<AspectRatioPreset, double> &ratios);
    /** Calculates image size suitable to passed @a hostSize and @a guestSize. */
    static QSize imageAspectRatioSize(const QSize &hostSize, const QSize &guestSize);

    /** @name Item stuff.
      * @{ */
        /** Holds the session reference. */
        CSession  m_comSession;
        /** Holds the machine reference. */
        CMachine  m_comMachine;

        /** Holds the update timer instance. */
        QTimer                                     *m_pUpdateTimer;
        /** Holds the update timer menu instance. */
        QMenu                                      *m_pUpdateTimerMenu;
        /** Holds the update timer menu action list. */
        QHash<PreviewUpdateIntervalType, QAction*>  m_actions;
    /** @} */

    /** @name Layout stuff.
      * @{ */
        /** Holds the aspect-ratio of the preview. */
        double     m_dRatio;
        /** Holds the layout margin. */
        const int  m_iMargin;
        /** Holds the viewport rectangle. */
        QRect      m_vRect;

        /** Holds the current aspect-ratio preset. */
        AspectRatioPreset                  m_enmPreset;
        /** Holds the aspect-ratio preset sizes. */
        QMap<AspectRatioPreset, QSize>     m_sizes;
        /** Holds the aspect-ratio preset ratios. */
        QMap<AspectRatioPreset, double>    m_ratios;
        /** Holds the aspect-ratio preset empty pixmaps. */
        QMap<AspectRatioPreset, QPixmap*>  m_emptyPixmaps;
        /** Holds the aspect-ratio preset filled pixmaps. */
        QMap<AspectRatioPreset, QPixmap*>  m_fullPixmaps;
    /** @} */

    /** @name Painting stuff.
      * @{ */
        /** Holds the preview image instance. */
        QImage  *m_pPreviewImg;
        /** Holds the preview name. */
        QString  m_strPreviewName;
    /** @} */
};

#endif /* !FEQT_INCLUDED_SRC_manager_details_UIMachinePreview_h */
