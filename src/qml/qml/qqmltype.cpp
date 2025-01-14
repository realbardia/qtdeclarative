/****************************************************************************
**
** Copyright (C) 2019 The Qt Company Ltd.
** Contact: https://www.qt.io/licensing/
**
** This file is part of the QtQml module of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 3 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL3 included in the
** packaging of this file. Please review the following information to
** ensure the GNU Lesser General Public License version 3 requirements
** will be met: https://www.gnu.org/licenses/lgpl-3.0.html.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 2.0 or (at your option) the GNU General
** Public license version 3 or any later version approved by the KDE Free
** Qt Foundation. The licenses are as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL2 and LICENSE.GPL3
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-2.0.html and
** https://www.gnu.org/licenses/gpl-3.0.html.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "qqmltype_p_p.h"

#include <QtQml/qjsvalue.h>
#include <QtQml/qqmlengine.h>
#include <QtQml/qqmlcontext.h>
#include <QtQml/qqmlcomponent.h>

#include <private/qqmlcustomparser_p.h>
#include <private/qqmldata_p.h>
#include <private/qqmlmetatypedata_p.h>
#include <private/qqmlpropertycache_p.h>
#include <private/qqmltypedata_p.h>

QT_BEGIN_NAMESPACE

QQmlTypePrivate::QQmlTypePrivate(QQmlType::RegistrationType type)
    : regType(type), iid(nullptr), typeId(0), listId(0), revision(0),
    containsRevisionedAttributes(false), baseMetaObject(nullptr),
    index(-1), isSetup(false), isEnumFromCacheSetup(false), isEnumFromBaseSetup(false),
    haveSuperType(false)
{
    switch (type) {
    case QQmlType::CppType:
        extraData.cd = new QQmlCppTypeData;
        extraData.cd->allocationSize = 0;
        extraData.cd->newFunc = nullptr;
        extraData.cd->parserStatusCast = -1;
        extraData.cd->extFunc = nullptr;
        extraData.cd->extMetaObject = nullptr;
        extraData.cd->customParser = nullptr;
        extraData.cd->attachedPropertiesFunc = nullptr;
        extraData.cd->attachedPropertiesType = nullptr;
        extraData.cd->propertyValueSourceCast = -1;
        extraData.cd->propertyValueInterceptorCast = -1;
        extraData.cd->registerEnumClassesUnscoped = true;
        break;
    case QQmlType::SingletonType:
    case QQmlType::CompositeSingletonType:
        extraData.sd = new QQmlSingletonTypeData;
        extraData.sd->singletonInstanceInfo = nullptr;
        break;
    case QQmlType::InterfaceType:
        extraData.cd = nullptr;
        break;
    case QQmlType::CompositeType:
        extraData.fd = new QQmlCompositeTypeData;
        break;
    default: qFatal("QQmlTypePrivate Internal Error.");
    }
}

QQmlTypePrivate::~QQmlTypePrivate()
{
    qDeleteAll(scopedEnums);
    switch (regType) {
    case QQmlType::CppType:
        delete extraData.cd->customParser;
        delete extraData.cd;
        break;
    case QQmlType::SingletonType:
    case QQmlType::CompositeSingletonType:
        delete extraData.sd->singletonInstanceInfo;
        delete extraData.sd;
        break;
    case QQmlType::CompositeType:
        delete extraData.fd;
        break;
    default: //Also InterfaceType, because it has no extra data
        break;
    }
}

QQmlType::QQmlType() = default;
QQmlType::QQmlType(const QQmlType &) = default;
QQmlType::QQmlType(QQmlType &&) = default;
QQmlType &QQmlType::operator =(const QQmlType &other) = default;
QQmlType &QQmlType::operator =(QQmlType &&other) = default;
QQmlType::QQmlType(const QQmlTypePrivate *priv) : d(priv) {}
QQmlType::~QQmlType() = default;

QHashedString QQmlType::module() const
{
    if (!d)
        return QHashedString();
    return d->module;
}

int QQmlType::majorVersion() const
{
    if (!d)
        return -1;
    return d->version_maj;
}

int QQmlType::minorVersion() const
{
    if (!d)
        return -1;
    return d->version_min;
}

bool QQmlType::availableInVersion(int vmajor, int vminor) const
{
    Q_ASSERT(vmajor >= 0 && vminor >= 0);
    if (!d)
        return false;
    return vmajor == d->version_maj && vminor >= d->version_min;
}

bool QQmlType::availableInVersion(const QHashedStringRef &module, int vmajor, int vminor) const
{
    Q_ASSERT(vmajor >= 0 && vminor >= 0);
    if (!d)
        return false;
    return module == d->module && vmajor == d->version_maj && vminor >= d->version_min;
}

QQmlType QQmlTypePrivate::resolveCompositeBaseType(QQmlEnginePrivate *engine) const
{
    Q_ASSERT(isComposite());
    if (!engine)
        return QQmlType();
    QQmlRefPointer<QQmlTypeData> td(engine->typeLoader.getType(sourceUrl()));
    if (td.isNull() || !td->isComplete())
        return QQmlType();
    QV4::ExecutableCompilationUnit *compilationUnit = td->compilationUnit();
    const QMetaObject *mo = compilationUnit->rootPropertyCache()->firstCppMetaObject();
    return QQmlMetaType::qmlType(mo);
}

QQmlPropertyCache *QQmlTypePrivate::compositePropertyCache(QQmlEnginePrivate *engine) const
{
    // similar logic to resolveCompositeBaseType
    Q_ASSERT(isComposite());
    if (!engine)
        return nullptr;
    QQmlRefPointer<QQmlTypeData> td(engine->typeLoader.getType(sourceUrl()));
    if (td.isNull() || !td->isComplete())
        return nullptr;
    QV4::ExecutableCompilationUnit *compilationUnit = td->compilationUnit();
    return compilationUnit->rootPropertyCache().data();
}

static bool isPropertyRevisioned(const QMetaObject *mo, int index)
{
    int i = index;
    i -= mo->propertyOffset();
    if (i < 0 && mo->d.superdata)
        return isPropertyRevisioned(mo->d.superdata, index);

    const QMetaObjectPrivate *mop = reinterpret_cast<const QMetaObjectPrivate*>(mo->d.data);
    if (i >= 0 && i < mop->propertyCount) {
        int handle = mop->propertyData + 3*i;
        int flags = mo->d.data[handle + 2];

        return (flags & Revisioned);
    }

    return false;
}

void QQmlTypePrivate::init() const
{
    if (isSetup)
        return;

    QMutexLocker lock(QQmlMetaType::typeRegistrationLock());
    if (isSetup)
        return;

    const QMetaObject *mo = baseMetaObject;
    if (!mo) {
        // version 0 singleton type without metaobject information
        return;
    }

    if (regType == QQmlType::CppType) {
        // Setup extended meta object
        // XXX - very inefficient
        if (extraData.cd->extFunc) {
            QMetaObjectBuilder builder;
            QQmlMetaType::clone(builder, extraData.cd->extMetaObject, extraData.cd->extMetaObject,
                                extraData.cd->extMetaObject);
            builder.setFlags(QMetaObjectBuilder::DynamicMetaObject);
            QMetaObject *mmo = builder.toMetaObject();
            mmo->d.superdata = mo;
            QQmlProxyMetaObject::ProxyData data = { mmo, extraData.cd->extFunc, 0, 0 };
            metaObjects << data;
        }
    }

    metaObjects.append(QQmlMetaType::proxyData(
            mo, baseMetaObject, metaObjects.isEmpty() ? nullptr
                                                      : metaObjects.constLast().metaObject));

    for (int ii = 0; ii < metaObjects.count(); ++ii) {
        metaObjects[ii].propertyOffset =
                metaObjects.at(ii).metaObject->propertyOffset();
        metaObjects[ii].methodOffset =
                metaObjects.at(ii).metaObject->methodOffset();
    }

    // Check for revisioned details
    {
        const QMetaObject *mo = nullptr;
        if (metaObjects.isEmpty())
            mo = baseMetaObject;
        else
            mo = metaObjects.constFirst().metaObject;

        for (int ii = 0; !containsRevisionedAttributes && ii < mo->propertyCount(); ++ii) {
            if (isPropertyRevisioned(mo, ii))
                containsRevisionedAttributes = true;
        }

        for (int ii = 0; !containsRevisionedAttributes && ii < mo->methodCount(); ++ii) {
            if (mo->method(ii).revision() != 0)
                containsRevisionedAttributes = true;
        }
    }

    isSetup = true;
    lock.unlock();
}

void QQmlTypePrivate::initEnums(QQmlEnginePrivate *engine) const
{
    const QQmlPropertyCache *cache = (!isEnumFromCacheSetup && isComposite())
            ? compositePropertyCache(engine)
            : nullptr;

    const QMetaObject *metaObject = !isEnumFromCacheSetup
            ? baseMetaObject // beware: It could be a singleton type without metaobject
            : nullptr;

    if (!cache && !metaObject)
        return;

    init();

    QMutexLocker lock(QQmlMetaType::typeRegistrationLock());

    if (cache) {
        insertEnumsFromPropertyCache(cache);
        isEnumFromCacheSetup = true;
    }

    if (metaObject) {
        insertEnums(metaObject);
        isEnumFromBaseSetup = true;
    }
}

void QQmlTypePrivate::insertEnums(const QMetaObject *metaObject) const
{
    // Add any enum values defined by 'related' classes
    if (metaObject->d.relatedMetaObjects) {
        const auto *related = metaObject->d.relatedMetaObjects;
        if (related) {
            while (*related)
                insertEnums(*related++);
        }
    }

    QSet<QString> localEnums;
    const QMetaObject *localMetaObject = nullptr;

    // Add any enum values defined by this class, overwriting any inherited values
    for (int ii = 0; ii < metaObject->enumeratorCount(); ++ii) {
        QMetaEnum e = metaObject->enumerator(ii);
        const bool isScoped = e.isScoped();
        QStringHash<int> *scoped = isScoped ? new QStringHash<int>() : nullptr;

    // We allow enums in sub-classes to overwrite enums from base-classes, such as
    // ListView.Center (from enum PositionMode) overwriting Item.Center (from enum TransformOrigin).
    // This is acceptable because the _use_ of the enum from the QML side requires qualification
    // anyway, i.e. ListView.Center vs. Item.Center.
        // However if a class defines two enums with the same value, then that must produce a warning
        // because it represents a valid conflict.
        if (e.enclosingMetaObject() != localMetaObject) {
            localEnums.clear();
            localMetaObject = e.enclosingMetaObject();
        }

        for (int jj = 0; jj < e.keyCount(); ++jj) {
            const QString key = QString::fromUtf8(e.key(jj));
            const int value = e.value(jj);
            if (!isScoped || (regType == QQmlType::CppType && extraData.cd->registerEnumClassesUnscoped)) {
                if (localEnums.contains(key)) {
                    auto existingEntry = enums.find(key);
                    if (existingEntry != enums.end() && existingEntry.value() != value) {
                        qWarning("Previously registered enum will be overwritten due to name clash: %s.%s", metaObject->className(), key.toUtf8().constData());
                        createEnumConflictReport(metaObject, key);
                    }
                } else {
                    localEnums.insert(key);
                }
                enums.insert(key, value);
            }
            if (isScoped)
                scoped->insert(key, value);
        }

        if (isScoped) {
            scopedEnums << scoped;
            scopedEnumIndex.insert(QString::fromUtf8(e.name()), scopedEnums.count()-1);
        }
    }
}

void QQmlTypePrivate::createListOfPossibleConflictingItems(const QMetaObject *metaObject, QList<EnumInfo> &enumInfoList, QStringList path) const
{
    path.append(QString::fromUtf8(metaObject->className()));

    if (metaObject->d.relatedMetaObjects) {
        const auto *related = metaObject->d.relatedMetaObjects;
        if (related) {
            while (*related)
                createListOfPossibleConflictingItems(*related++, enumInfoList, path);
        }
    }

    for (int ii = 0; ii < metaObject->enumeratorCount(); ++ii) {
        const auto e = metaObject->enumerator(ii);

        for (int jj = 0; jj < e.keyCount(); ++jj) {
            const QString key = QString::fromUtf8(e.key(jj));

            EnumInfo enumInfo;
            enumInfo.metaObjectName = QString::fromUtf8(metaObject->className());
            enumInfo.enumName = QString::fromUtf8(e.name());
            enumInfo.enumKey = key;
            enumInfo.scoped = e.isScoped();
            enumInfo.path = path;
            enumInfo.metaEnumScope = QString::fromUtf8(e.scope());
            enumInfoList.append(enumInfo);
        }
    }
}

void QQmlTypePrivate::createEnumConflictReport(const QMetaObject *metaObject, const QString &conflictingKey) const
{
    QList<EnumInfo> enumInfoList;

    if (baseMetaObject) // prefer baseMetaObject if available
        metaObject = baseMetaObject;

    if (!metaObject) { // If there is no metaObject at all return early
        qWarning() << "No meta object information available. Skipping conflict analysis.";
        return;
    }

    createListOfPossibleConflictingItems(metaObject, enumInfoList, QStringList());

    qWarning().noquote() << QLatin1String("Possible conflicting items:");
    // find items with conflicting key
    for (const auto i : enumInfoList) {
        if (i.enumKey == conflictingKey)
        qWarning().noquote().nospace() << "    " << i.metaObjectName << "." << i.enumName << "." << i.enumKey << " from scope "
                                           << i.metaEnumScope << " injected by " << i.path.join(QLatin1String("->"));
    }
}

void QQmlTypePrivate::insertEnumsFromPropertyCache(const QQmlPropertyCache *cache) const
{
    const QMetaObject *cppMetaObject = cache->firstCppMetaObject();

    while (cache && cache->metaObject() != cppMetaObject) {

        int count = cache->qmlEnumCount();
        for (int ii = 0; ii < count; ++ii) {
            QStringHash<int> *scoped = new QStringHash<int>();
            QQmlEnumData *enumData = cache->qmlEnum(ii);

            for (int jj = 0; jj < enumData->values.count(); ++jj) {
                const QQmlEnumValue &value = enumData->values.at(jj);
                enums.insert(value.namedValue, value.value);
                scoped->insert(value.namedValue, value.value);
            }
            scopedEnums << scoped;
            scopedEnumIndex.insert(enumData->name, scopedEnums.count()-1);
        }
        cache = cache->parent();
    }
    insertEnums(cppMetaObject);
}

void QQmlTypePrivate::setName(const QString &uri, const QString &element)
{
    module = uri;
    elementName = element;
    name = uri.isEmpty() ? element : (uri + QLatin1Char('/') + element);
}

QByteArray QQmlType::typeName() const
{
    if (d) {
        if (d->regType == SingletonType || d->regType == CompositeSingletonType)
            return d->extraData.sd->singletonInstanceInfo->typeName.toUtf8();
        else if (d->baseMetaObject)
            return d->baseMetaObject->className();
    }
    return QByteArray();
}

QString QQmlType::elementName() const
{
    if (!d)
        return QString();
    return d->elementName;
}

QString QQmlType::qmlTypeName() const
{
    if (!d)
        return QString();
    return d->name;
}

QObject *QQmlType::create() const
{
    if (!d || !isCreatable())
        return nullptr;

    d->init();

    QObject *rv = (QObject *)operator new(d->extraData.cd->allocationSize);
    d->extraData.cd->newFunc(rv);

    if (rv && !d->metaObjects.isEmpty())
        (void)new QQmlProxyMetaObject(rv, &d->metaObjects);

    return rv;
}

void QQmlType::create(QObject **out, void **memory, size_t additionalMemory) const
{
    if (!d || !isCreatable())
        return;

    d->init();

    QObject *rv = (QObject *)operator new(d->extraData.cd->allocationSize + additionalMemory);
    d->extraData.cd->newFunc(rv);

    if (rv && !d->metaObjects.isEmpty())
        (void)new QQmlProxyMetaObject(rv, &d->metaObjects);

    *out = rv;
    *memory = ((char *)rv) + d->extraData.cd->allocationSize;
}

QQmlType::SingletonInstanceInfo *QQmlType::singletonInstanceInfo() const
{
    if (!d)
        return nullptr;
    if (d->regType != SingletonType && d->regType != CompositeSingletonType)
        return nullptr;
    return d->extraData.sd->singletonInstanceInfo;
}

QQmlCustomParser *QQmlType::customParser() const
{
    if (!d)
        return nullptr;
    if (d->regType != CppType)
        return nullptr;
    return d->extraData.cd->customParser;
}

QQmlType::CreateFunc QQmlType::createFunction() const
{
    if (!d || d->regType != CppType)
        return nullptr;
    return d->extraData.cd->newFunc;
}

QString QQmlType::noCreationReason() const
{
    if (!d || d->regType != CppType)
        return QString();
    return d->extraData.cd->noCreationReason;
}

bool QQmlType::isCreatable() const
{
    return d && d->regType == CppType && d->extraData.cd->newFunc;
}

QQmlType::ExtensionFunc QQmlType::extensionFunction() const
{
    if (!d || d->regType != CppType)
        return nullptr;
    return d->extraData.cd->extFunc;
}

bool QQmlType::isExtendedType() const
{
    if (!d)
        return false;
    d->init();

    return !d->metaObjects.isEmpty();
}

bool QQmlType::isSingleton() const
{
    return d && (d->regType == SingletonType || d->regType == CompositeSingletonType);
}

bool QQmlType::isInterface() const
{
    return d && d->regType == InterfaceType;
}

bool QQmlType::isComposite() const
{
    return d && d->isComposite();
}

bool QQmlType::isCompositeSingleton() const
{
    return d && d->regType == CompositeSingletonType;
}

bool QQmlType::isQObjectSingleton() const
{
    return d && d->regType == SingletonType && d->extraData.sd->singletonInstanceInfo->qobjectCallback;
}

bool QQmlType::isQJSValueSingleton() const
{
    return d && d->regType == SingletonType && d->extraData.sd->singletonInstanceInfo->scriptCallback;
}

int QQmlType::typeId() const
{
    return d ? d->typeId : -1;
}

int QQmlType::qListTypeId() const
{
    return d ? d->listId : -1;
}

const QMetaObject *QQmlType::metaObject() const
{
    if (!d)
        return nullptr;
    d->init();

    if (d->metaObjects.isEmpty())
        return d->baseMetaObject;
    else
        return d->metaObjects.constFirst().metaObject;

}

const QMetaObject *QQmlType::baseMetaObject() const
{
    return d ? d->baseMetaObject : nullptr;
}

bool QQmlType::containsRevisionedAttributes() const
{
    if (!d)
        return false;
    d->init();

    return d->containsRevisionedAttributes;
}

int QQmlType::metaObjectRevision() const
{
    return d ? d->revision : -1;
}

QQmlAttachedPropertiesFunc QQmlType::attachedPropertiesFunction(QQmlEnginePrivate *engine) const
{
    if (!d)
        return nullptr;
    if (d->regType == CppType)
        return d->extraData.cd->attachedPropertiesFunc;

    QQmlType base;
    if (d->regType == CompositeType)
        base = d->resolveCompositeBaseType(engine);
    return base.attachedPropertiesFunction(engine);
}

const QMetaObject *QQmlType::attachedPropertiesType(QQmlEnginePrivate *engine) const
{
    if (!d)
        return nullptr;
    if (d->regType == CppType)
        return d->extraData.cd->attachedPropertiesType;

    QQmlType base;
    if (d->regType == CompositeType)
        base = d->resolveCompositeBaseType(engine);
    return base.attachedPropertiesType(engine);
}

#if QT_DEPRECATED_SINCE(5, 14)
/*
This is the id passed to qmlAttachedPropertiesById().  This is different from the index
for the case that a single class is registered under two or more names (eg. Item in
Qt 4.7 and QtQuick 1.0).
*/
int QQmlType::attachedPropertiesId(QQmlEnginePrivate *engine) const
{
    if (!d)
        return -1;
    if (d->regType == CppType)
        return d->extraData.cd->attachedPropertiesType ? d->index : -1;

    QQmlType base;
    if (d->regType == CompositeType)
        base = d->resolveCompositeBaseType(engine);
    return base.attachedPropertiesId(engine);
}
#endif

int QQmlType::parserStatusCast() const
{
    if (!d || d->regType != CppType)
        return -1;
    return d->extraData.cd->parserStatusCast;
}

int QQmlType::propertyValueSourceCast() const
{
    if (!d || d->regType != CppType)
        return -1;
    return d->extraData.cd->propertyValueSourceCast;
}

int QQmlType::propertyValueInterceptorCast() const
{
    if (!d || d->regType != CppType)
        return -1;
    return d->extraData.cd->propertyValueInterceptorCast;
}

const char *QQmlType::interfaceIId() const
{
    if (!d || d->regType != InterfaceType)
        return nullptr;
    return d->iid;
}

int QQmlType::index() const
{
    return d ? d->index : -1;
}

QUrl QQmlType::sourceUrl() const
{
    return d ? d->sourceUrl() : QUrl();
}

int QQmlType::enumValue(QQmlEnginePrivate *engine, const QHashedStringRef &name, bool *ok) const
{
    Q_ASSERT(ok);
    if (d) {
        *ok = true;

        d->initEnums(engine);

        int *rv = d->enums.value(name);
        if (rv)
            return *rv;
    }

    *ok = false;
    return -1;
}

int QQmlType::enumValue(QQmlEnginePrivate *engine, const QHashedCStringRef &name, bool *ok) const
{
    Q_ASSERT(ok);
    if (d) {
        *ok = true;

        d->initEnums(engine);

        int *rv = d->enums.value(name);
        if (rv)
            return *rv;
    }

    *ok = false;
    return -1;
}

int QQmlType::enumValue(QQmlEnginePrivate *engine, const QV4::String *name, bool *ok) const
{
    Q_ASSERT(ok);
    if (d) {
        *ok = true;

        d->initEnums(engine);

        int *rv = d->enums.value(name);
        if (rv)
            return *rv;
    }

    *ok = false;
    return -1;
}

int QQmlType::scopedEnumIndex(QQmlEnginePrivate *engine, const QV4::String *name, bool *ok) const
{
    Q_ASSERT(ok);
    if (d) {
        *ok = true;

        d->initEnums(engine);

        int *rv = d->scopedEnumIndex.value(name);
        if (rv)
            return *rv;
    }

    *ok = false;
    return -1;
}

int QQmlType::scopedEnumIndex(QQmlEnginePrivate *engine, const QString &name, bool *ok) const
{
    Q_ASSERT(ok);
    if (d) {
        *ok = true;

        d->initEnums(engine);

        int *rv = d->scopedEnumIndex.value(name);
        if (rv)
            return *rv;
    }

    *ok = false;
    return -1;
}

int QQmlType::scopedEnumValue(QQmlEnginePrivate *engine, int index, const QV4::String *name, bool *ok) const
{
    Q_UNUSED(engine)
    Q_ASSERT(ok);
    *ok = true;

    if (d) {
        Q_ASSERT(index > -1 && index < d->scopedEnums.count());
        int *rv = d->scopedEnums.at(index)->value(name);
        if (rv)
            return *rv;
    }

    *ok = false;
    return -1;
}

int QQmlType::scopedEnumValue(QQmlEnginePrivate *engine, int index, const QString &name, bool *ok) const
{
    Q_UNUSED(engine)
    Q_ASSERT(ok);
    *ok = true;

    if (d) {
        Q_ASSERT(index > -1 && index < d->scopedEnums.count());
        int *rv = d->scopedEnums.at(index)->value(name);
        if (rv)
            return *rv;
    }

    *ok = false;
    return -1;
}

int QQmlType::scopedEnumValue(QQmlEnginePrivate *engine, const QByteArray &scopedEnumName, const QByteArray &name, bool *ok) const
{
    Q_ASSERT(ok);
    if (d) {
        *ok = true;

        d->initEnums(engine);

        int *rv = d->scopedEnumIndex.value(QHashedCStringRef(scopedEnumName.constData(), scopedEnumName.length()));
        if (rv) {
            int index = *rv;
            Q_ASSERT(index > -1 && index < d->scopedEnums.count());
            rv = d->scopedEnums.at(index)->value(QHashedCStringRef(name.constData(), name.length()));
            if (rv)
                return *rv;
        }
    }

    *ok = false;
    return -1;
}

int QQmlType::scopedEnumValue(QQmlEnginePrivate *engine, const QStringRef &scopedEnumName, const QStringRef &name, bool *ok) const
{
    Q_ASSERT(ok);
    if (d) {
        *ok = true;

        d->initEnums(engine);

        int *rv = d->scopedEnumIndex.value(QHashedStringRef(scopedEnumName));
        if (rv) {
            int index = *rv;
            Q_ASSERT(index > -1 && index < d->scopedEnums.count());
            rv = d->scopedEnums.at(index)->value(QHashedStringRef(name));
            if (rv)
                return *rv;
        }
    }

    *ok = false;
    return -1;
}

void QQmlType::refHandle(const QQmlTypePrivate *priv)
{
    if (priv)
        priv->addref();
}

void QQmlType::derefHandle(const QQmlTypePrivate *priv)
{
    if (priv)
        priv->release();
}

int QQmlType::refCount(const QQmlTypePrivate *priv)
{
    if (priv)
        return priv->count();
    return -1;
}

QT_END_NAMESPACE
