/****************************************************************************
**
** Copyright (C) 2010 Nokia Corporation and/or its subsidiary(-ies).
** All rights reserved.
** Contact: Nokia Corporation (qt-info@nokia.com)
**
** This file is part of the test suite of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL$
** GNU Lesser General Public License Usage
** This file may be used under the terms of the GNU Lesser General Public
** License version 2.1 as published by the Free Software Foundation and
** appearing in the file LICENSE.LGPL included in the packaging of this
** file. Please review the following information to ensure the GNU Lesser
** General Public License version 2.1 requirements will be met:
** http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Nokia gives you certain additional
** rights. These rights are described in the Nokia Qt LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU General
** Public License version 3.0 as published by the Free Software Foundation
** and appearing in the file LICENSE.GPL included in the packaging of this
** file. Please review the following information to ensure the GNU General
** Public License version 3.0 requirements will be met:
** http://www.gnu.org/copyleft/gpl.html.
**
** Other Usage
** Alternatively, this file may be used in accordance with the terms and
** conditions contained in a signed written agreement between you and Nokia.
**
**
**
**
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include <QtDeclarative/qdeclarativeextensionplugin.h>
#include <QtDeclarative/qdeclarative.h>
#include <QtDeclarative/qjsvalue.h>
#include <QtDeclarative/qjsengine.h>
#include "QtQuickTest/private/quicktestresult_p.h"
#include "QtQuickTest/private/quicktestevent_p.h"
#include "private/qtestoptions_p.h"
#include "QtDeclarative/qquickitem.h"
#include <QtDeclarative/private/qdeclarativeengine_p.h>

QML_DECLARE_TYPE(QuickTestResult)
QML_DECLARE_TYPE(QuickTestEvent)

#include <QtDebug>

QT_BEGIN_NAMESPACE

class QuickTestUtil : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool printAvailableFunctions READ printAvailableFunctions NOTIFY printAvailableFunctionsChanged)
    Q_PROPERTY(bool wrapper READ wrapper NOTIFY wrapperChanged)
public:
    QuickTestUtil(QObject *parent = 0)
        :QObject(parent)
    {}

    ~QuickTestUtil()
    {}
    bool printAvailableFunctions() const
    {
        return QTest::printAvailableFunctions;
    }
    bool wrapper() const
    {
        return true;
    }
Q_SIGNALS:
    void printAvailableFunctionsChanged();
    void wrapperChanged();
public Q_SLOTS:

    QDeclarativeV8Handle typeName(const QVariant& v) const
    {
        QString name(v.typeName());
        //qDebug() << "type:" << name  << " string value:" << v.toString() << " value:" << v;
        if (v.canConvert<QObject*>()) {
            QDeclarativeType *type = 0;
            const QMetaObject *mo = v.value<QObject*>()->metaObject();
            while (!type && mo) {
                type = QDeclarativeMetaType::qmlType(mo);
                mo = mo->superClass();
            }
            if (type) {
                name = type->qmlTypeName();
            }
        }

        return QDeclarativeV8Handle::fromHandle(v8::String::New(name.toUtf8()));
    }

    bool compare(const QVariant& act, const QVariant& exp) const {
        return act == exp;
    }

    QDeclarativeV8Handle callerFile(int frameIndex = 0) const
    {
        v8::Local<v8::StackTrace> stacks = v8::StackTrace::CurrentStackTrace(10, v8::StackTrace::kDetailed);
        int count = stacks->GetFrameCount();
        if (count >= frameIndex + 1) {
            v8::Local<v8::StackFrame> frame = stacks->GetFrame(frameIndex + 1);
            return QDeclarativeV8Handle::fromHandle(frame->GetScriptNameOrSourceURL());
        }
        return QDeclarativeV8Handle();
    }
    int callerLine(int frameIndex = 0) const
    {
        v8::Local<v8::StackTrace> stacks = v8::StackTrace::CurrentStackTrace(10, v8::StackTrace::kDetailed);
        int count = stacks->GetFrameCount();
        if (count >= frameIndex + 1) {
            v8::Local<v8::StackFrame> frame = stacks->GetFrame(frameIndex + 1);
            return frame->GetLineNumber();
        }
        return -1;
    }
};

QT_END_NAMESPACE

QML_DECLARE_TYPE(QuickTestUtil)

QT_BEGIN_NAMESPACE

class QTestQmlModule : public QDeclarativeExtensionPlugin
{
    Q_OBJECT
public:
    virtual void registerTypes(const char *uri)
    {
        Q_ASSERT(QLatin1String(uri) == QLatin1String("QtTest"));
        qmlRegisterType<QuickTestResult>(uri,1,0,"TestResult");
        qmlRegisterType<QuickTestEvent>(uri,1,0,"TestEvent");
        qmlRegisterType<QuickTestUtil>(uri,1,0,"TestUtil");
    }

    void initializeEngine(QDeclarativeEngine *, const char *)
    {
    }
};

QT_END_NAMESPACE

#include "main.moc"

Q_EXPORT_PLUGIN2(qmltestplugin, QT_PREPEND_NAMESPACE(QTestQmlModule))
