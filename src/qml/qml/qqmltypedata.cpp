/****************************************************************************
**
** Copyright (C) 2016 The Qt Company Ltd.
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

#include <private/qqmltypedata_p.h>
#include <private/qqmlengine_p.h>
#include <private/qqmlpropertycachecreator_p.h>
#include <private/qqmlpropertyvalidator_p.h>
#include <private/qqmlirbuilder_p.h>
#include <private/qqmlirloader_p.h>
#include <private/qqmlscriptblob_p.h>
#include <private/qqmlscriptdata_p.h>
#include <private/qqmltypecompiler_p.h>

#include <QtCore/qloggingcategory.h>
#include <QtCore/qcryptographichash.h>

Q_DECLARE_LOGGING_CATEGORY(DBG_DISK_CACHE)

QT_BEGIN_NAMESPACE

QQmlTypeData::TypeDataCallback::~TypeDataCallback()
{
}

QString QQmlTypeData::TypeReference::qualifiedName() const
{
    QString result;
    if (!prefix.isEmpty()) {
        result = prefix + QLatin1Char('.');
    }
    result.append(type.qmlTypeName());
    return result;
}

QQmlTypeData::QQmlTypeData(const QUrl &url, QQmlTypeLoader *manager)
    : QQmlTypeLoader::Blob(url, QmlFile, manager),
      m_typesResolved(false), m_implicitImportLoaded(false)
{

}

QQmlTypeData::~QQmlTypeData()
{
    m_scripts.clear();
    m_compositeSingletons.clear();
    m_resolvedTypes.clear();
}

const QList<QQmlTypeData::ScriptReference> &QQmlTypeData::resolvedScripts() const
{
    return m_scripts;
}

QV4::ExecutableCompilationUnit *QQmlTypeData::compilationUnit() const
{
    return m_compiledData.data();
}

void QQmlTypeData::registerCallback(TypeDataCallback *callback)
{
    Q_ASSERT(!m_callbacks.contains(callback));
    m_callbacks.append(callback);
}

void QQmlTypeData::unregisterCallback(TypeDataCallback *callback)
{
    Q_ASSERT(m_callbacks.contains(callback));
    m_callbacks.removeOne(callback);
    Q_ASSERT(!m_callbacks.contains(callback));
}

bool QQmlTypeData::tryLoadFromDiskCache()
{
    if (diskCacheDisabled() && !diskCacheForced())
        return false;

    if (isDebugging())
        return false;

    QV4::ExecutionEngine *v4 = typeLoader()->engine()->handle();
    if (!v4)
        return false;

    QQmlRefPointer<QV4::ExecutableCompilationUnit> unit = QV4::ExecutableCompilationUnit::create();
    {
        QString error;
        if (!unit->loadFromDisk(url(), m_backupSourceCode.sourceTimeStamp(), &error)) {
            qCDebug(DBG_DISK_CACHE) << "Error loading" << urlString() << "from disk cache:" << error;
            return false;
        }
    }

    if (unit->unitData()->flags & QV4::CompiledData::Unit::PendingTypeCompilation) {
        restoreIR(std::move(*unit));
        return true;
    }

    m_compiledData = unit;

    for (int i = 0, count = m_compiledData->objectCount(); i < count; ++i)
        m_typeReferences.collectFromObject(m_compiledData->objectAt(i));

    m_importCache.setBaseUrl(finalUrl(), finalUrlString());

    // For remote URLs, we don't delay the loading of the implicit import
    // because the loading probably requires an asynchronous fetch of the
    // qmldir (so we can't load it just in time).
    if (!finalUrl().scheme().isEmpty()) {
        QUrl qmldirUrl = finalUrl().resolved(QUrl(QLatin1String("qmldir")));
        if (!QQmlImports::isLocal(qmldirUrl)) {
            if (!loadImplicitImport())
                return false;

            // find the implicit import
            for (quint32 i = 0, count = m_compiledData->importCount(); i < count; ++i) {
                const QV4::CompiledData::Import *import = m_compiledData->importAt(i);
                if (m_compiledData->stringAt(import->uriIndex) == QLatin1String(".")
                    && import->qualifierIndex == 0
                    && import->majorVersion == -1
                    && import->minorVersion == -1) {
                    QList<QQmlError> errors;
                    auto pendingImport = std::make_shared<PendingImport>(this, import);
                    if (!fetchQmldir(qmldirUrl, pendingImport, 1, &errors)) {
                        setError(errors);
                        return false;
                    }
                    break;
                }
            }
        }
    }

    for (int i = 0, count = m_compiledData->importCount(); i < count; ++i) {
        const QV4::CompiledData::Import *import = m_compiledData->importAt(i);
        QList<QQmlError> errors;
        if (!addImport(import, &errors)) {
            Q_ASSERT(errors.size());
            QQmlError error(errors.takeFirst());
            error.setUrl(m_importCache.baseUrl());
            error.setLine(import->location.line);
            error.setColumn(import->location.column);
            errors.prepend(error); // put it back on the list after filling out information.
            setError(errors);
            return false;
        }
    }

    return true;
}

void QQmlTypeData::createTypeAndPropertyCaches(
        const QQmlRefPointer<QQmlTypeNameCache> &typeNameCache,
        const QV4::ResolvedTypeReferenceMap &resolvedTypeCache)
{
    Q_ASSERT(m_compiledData);
    m_compiledData->typeNameCache = typeNameCache;
    m_compiledData->resolvedTypes = resolvedTypeCache;

    QQmlEnginePrivate * const engine = QQmlEnginePrivate::get(typeLoader()->engine());

    QQmlPendingGroupPropertyBindings pendingGroupPropertyBindings;

    {
        QQmlPropertyCacheCreator<QV4::ExecutableCompilationUnit> propertyCacheCreator(
                &m_compiledData->propertyCaches, &pendingGroupPropertyBindings, engine,
                m_compiledData.data(), &m_importCache);
        QQmlJS::DiagnosticMessage error = propertyCacheCreator.buildMetaObjects();
        if (error.isValid()) {
            setError(error);
            return;
        }
    }

    QQmlPropertyCacheAliasCreator<QV4::ExecutableCompilationUnit> aliasCreator(
            &m_compiledData->propertyCaches, m_compiledData.data());
    aliasCreator.appendAliasPropertiesToMetaObjects();

    pendingGroupPropertyBindings.resolveMissingPropertyCaches(engine, &m_compiledData->propertyCaches);
}

static bool addTypeReferenceChecksumsToHash(const QList<QQmlTypeData::TypeReference> &typeRefs, QCryptographicHash *hash, QQmlEngine *engine)
{
    for (const auto &typeRef: typeRefs) {
        if (typeRef.typeData) {
            const auto unit = typeRef.typeData->compilationUnit()->unitData();
            hash->addData(unit->md5Checksum, sizeof(unit->md5Checksum));
        } else if (typeRef.type.isValid()) {
            const auto propertyCache = QQmlEnginePrivate::get(engine)->cache(typeRef.type.metaObject());
            bool ok = false;
            hash->addData(propertyCache->checksum(&ok));
            if (!ok)
                return false;
        }
    }
    return true;
}

void QQmlTypeData::done()
{
    auto cleanup = qScopeGuard([this]{
        m_document.reset();
        m_typeReferences.clear();
        if (isError())
            m_compiledData = nullptr;
    });

    if (isError())
        return;

    // Check all script dependencies for errors
    for (int ii = 0; ii < m_scripts.count(); ++ii) {
        const ScriptReference &script = m_scripts.at(ii);
        Q_ASSERT(script.script->isCompleteOrError());
        if (script.script->isError()) {
            QList<QQmlError> errors = script.script->errors();
            QQmlError error;
            error.setUrl(url());
            error.setLine(script.location.line);
            error.setColumn(script.location.column);
            error.setDescription(QQmlTypeLoader::tr("Script %1 unavailable").arg(script.script->urlString()));
            errors.prepend(error);
            setError(errors);
            return;
        }
    }

    // Check all type dependencies for errors
    for (auto it = m_resolvedTypes.constBegin(), end = m_resolvedTypes.constEnd(); it != end;
         ++it) {
        const TypeReference &type = *it;
        Q_ASSERT(!type.typeData || type.typeData->isCompleteOrError());
        if (type.typeData && type.typeData->isError()) {
            const QString typeName = stringAt(it.key());

            QList<QQmlError> errors = type.typeData->errors();
            QQmlError error;
            error.setUrl(url());
            error.setLine(type.location.line);
            error.setColumn(type.location.column);
            error.setDescription(QQmlTypeLoader::tr("Type %1 unavailable").arg(typeName));
            errors.prepend(error);
            setError(errors);
            return;
        }
    }

    // Check all composite singleton type dependencies for errors
    for (int ii = 0; ii < m_compositeSingletons.count(); ++ii) {
        const TypeReference &type = m_compositeSingletons.at(ii);
        Q_ASSERT(!type.typeData || type.typeData->isCompleteOrError());
        if (type.typeData && type.typeData->isError()) {
            QString typeName = type.type.qmlTypeName();

            QList<QQmlError> errors = type.typeData->errors();
            QQmlError error;
            error.setUrl(url());
            error.setLine(type.location.line);
            error.setColumn(type.location.column);
            error.setDescription(QQmlTypeLoader::tr("Type %1 unavailable").arg(typeName));
            errors.prepend(error);
            setError(errors);
            return;
        }
    }

    QQmlRefPointer<QQmlTypeNameCache> typeNameCache;
    QV4::ResolvedTypeReferenceMap resolvedTypeCache;
    {
        QQmlJS::DiagnosticMessage error = buildTypeResolutionCaches(&typeNameCache, &resolvedTypeCache);
        if (error.isValid()) {
            setError(error);
            return;
        }
    }

    QQmlEngine *const engine = typeLoader()->engine();

    const auto dependencyHasher = [engine, &resolvedTypeCache, this]() {
        QCryptographicHash hash(QCryptographicHash::Md5);
        return (resolvedTypeCache.addToHash(&hash, engine)
                && ::addTypeReferenceChecksumsToHash(m_compositeSingletons, &hash, engine))
                ? hash.result()
                : QByteArray();
    };

    // verify if any dependencies changed if we're using a cache
    if (m_document.isNull() && !m_compiledData->verifyChecksum(dependencyHasher)) {
        qCDebug(DBG_DISK_CACHE) << "Checksum mismatch for cached version of" << m_compiledData->fileName();
        if (!loadFromSource())
            return;
        m_backupSourceCode = SourceCodeData();
        m_compiledData = nullptr;
    }

    if (!m_document.isNull()) {
        // Compile component
        compile(typeNameCache, &resolvedTypeCache, dependencyHasher);
    } else {
        createTypeAndPropertyCaches(typeNameCache, resolvedTypeCache);
    }

    if (isError())
        return;

    {
        QQmlEnginePrivate *const enginePrivate = QQmlEnginePrivate::get(engine);
        {
            // Sanity check property bindings
            QQmlPropertyValidator validator(enginePrivate, m_importCache, m_compiledData);
            QVector<QQmlJS::DiagnosticMessage> errors = validator.validate();
            if (!errors.isEmpty()) {
                setError(errors);
                return;
            }
        }

        m_compiledData->finalizeCompositeType(enginePrivate);
    }

    {
        QQmlType type = QQmlMetaType::qmlType(finalUrl(), true);
        if (m_compiledData && m_compiledData->unitData()->flags & QV4::CompiledData::Unit::IsSingleton) {
            if (!type.isValid()) {
                QQmlError error;
                error.setDescription(QQmlTypeLoader::tr("No matching type found, pragma Singleton files cannot be used by QQmlComponent."));
                setError(error);
                return;
            } else if (!type.isCompositeSingleton()) {
                QQmlError error;
                error.setDescription(QQmlTypeLoader::tr("pragma Singleton used with a non composite singleton type %1").arg(type.qmlTypeName()));
                setError(error);
                return;
            }
        } else {
            // If the type is CompositeSingleton but there was no pragma Singleton in the
            // QML file, lets report an error.
            if (type.isValid() && type.isCompositeSingleton()) {
                QString typeName = type.qmlTypeName();
                setError(QQmlTypeLoader::tr("qmldir defines type as singleton, but no pragma Singleton found in type %1.").arg(typeName));
                return;
            }
        }
    }

    {
        // Collect imported scripts
        m_compiledData->dependentScripts.reserve(m_scripts.count());
        for (int scriptIndex = 0; scriptIndex < m_scripts.count(); ++scriptIndex) {
            const QQmlTypeData::ScriptReference &script = m_scripts.at(scriptIndex);

            QStringRef qualifier(&script.qualifier);
            QString enclosingNamespace;

            const int lastDotIndex = qualifier.lastIndexOf(QLatin1Char('.'));
            if (lastDotIndex != -1) {
                enclosingNamespace = qualifier.left(lastDotIndex).toString();
                qualifier = qualifier.mid(lastDotIndex+1);
            }

            m_compiledData->typeNameCache->add(qualifier.toString(), scriptIndex, enclosingNamespace);
            QQmlRefPointer<QQmlScriptData> scriptData = script.script->scriptData();
            m_compiledData->dependentScripts << scriptData;
        }
    }
}

void QQmlTypeData::completed()
{
    // Notify callbacks
    while (!m_callbacks.isEmpty()) {
        TypeDataCallback *callback = m_callbacks.takeFirst();
        callback->typeDataReady(this);
    }
}

bool QQmlTypeData::loadImplicitImport()
{
    m_implicitImportLoaded = true; // Even if we hit an error, count as loaded (we'd just keep hitting the error)

    m_importCache.setBaseUrl(finalUrl(), finalUrlString());

    QQmlImportDatabase *importDatabase = typeLoader()->importDatabase();
    // For local urls, add an implicit import "." as most overridden lookup.
    // This will also trigger the loading of the qmldir and the import of any native
    // types from available plugins.
    QList<QQmlError> implicitImportErrors;
    m_importCache.addImplicitImport(importDatabase, &implicitImportErrors);

    if (!implicitImportErrors.isEmpty()) {
        setError(implicitImportErrors);
        return false;
    }

    return true;
}

void QQmlTypeData::dataReceived(const SourceCodeData &data)
{
    m_backupSourceCode = data;

    if (tryLoadFromDiskCache())
        return;

    if (isError())
        return;

    if (!m_backupSourceCode.exists() || m_backupSourceCode.isEmpty()) {
        if (m_cachedUnitStatus == QQmlMetaType::CachedUnitLookupError::VersionMismatch)
            setError(QQmlTypeLoader::tr("File was compiled ahead of time with an incompatible version of Qt and the original file cannot be found. Please recompile"));
        else if (!m_backupSourceCode.exists())
            setError(QQmlTypeLoader::tr("No such file or directory"));
        else
            setError(QQmlTypeLoader::tr("File is empty"));
        return;
    }

    if (!loadFromSource())
        return;

    continueLoadFromIR();
}

void QQmlTypeData::initializeFromCachedUnit(const QV4::CompiledData::Unit *unit)
{
    m_document.reset(new QmlIR::Document(isDebugging()));
    QQmlIRLoader loader(unit, m_document.data());
    loader.load();
    m_document->jsModule.fileName = urlString();
    m_document->jsModule.finalUrl = finalUrlString();
    m_document->javaScriptCompilationUnit = QV4::CompiledData::CompilationUnit(unit);
    continueLoadFromIR();
}

bool QQmlTypeData::loadFromSource()
{
    m_document.reset(new QmlIR::Document(isDebugging()));
    m_document->jsModule.sourceTimeStamp = m_backupSourceCode.sourceTimeStamp();
    QQmlEngine *qmlEngine = typeLoader()->engine();
    QmlIR::IRBuilder compiler(qmlEngine->handle()->illegalNames());

    QString sourceError;
    const QString source = m_backupSourceCode.readAll(&sourceError);
    if (!sourceError.isEmpty()) {
        setError(sourceError);
        return false;
    }

    if (!compiler.generateFromQml(source, finalUrlString(), m_document.data())) {
        QList<QQmlError> errors;
        errors.reserve(compiler.errors.count());
        for (const QQmlJS::DiagnosticMessage &msg : qAsConst(compiler.errors)) {
            QQmlError e;
            e.setUrl(url());
            e.setLine(msg.line);
            e.setColumn(msg.column);
            e.setDescription(msg.message);
            errors << e;
        }
        setError(errors);
        return false;
    }
    return true;
}

void QQmlTypeData::restoreIR(QV4::CompiledData::CompilationUnit &&unit)
{
    m_document.reset(new QmlIR::Document(isDebugging()));
    QQmlIRLoader loader(unit.unitData(), m_document.data());
    loader.load();
    m_document->jsModule.fileName = urlString();
    m_document->jsModule.finalUrl = finalUrlString();
    m_document->javaScriptCompilationUnit = std::move(unit);
    continueLoadFromIR();
}

void QQmlTypeData::continueLoadFromIR()
{
    m_typeReferences.collectFromObjects(m_document->objects.constBegin(), m_document->objects.constEnd());
    m_importCache.setBaseUrl(finalUrl(), finalUrlString());

    // For remote URLs, we don't delay the loading of the implicit import
    // because the loading probably requires an asynchronous fetch of the
    // qmldir (so we can't load it just in time).
    if (!finalUrl().scheme().isEmpty()) {
        QUrl qmldirUrl = finalUrl().resolved(QUrl(QLatin1String("qmldir")));
        if (!QQmlImports::isLocal(qmldirUrl)) {
            if (!loadImplicitImport())
                return;
            // This qmldir is for the implicit import
            auto implicitImport = std::make_shared<PendingImport>();
            implicitImport->uri = QLatin1String(".");
            implicitImport->majorVersion = -1;
            implicitImport->minorVersion = -1;
            QList<QQmlError> errors;

            if (!fetchQmldir(qmldirUrl, implicitImport, 1, &errors)) {
                setError(errors);
                return;
            }
        }
    }

    QList<QQmlError> errors;

    for (const QV4::CompiledData::Import *import : qAsConst(m_document->imports)) {
        if (!addImport(import, &errors)) {
            Q_ASSERT(errors.size());
            QQmlError error(errors.takeFirst());
            error.setUrl(m_importCache.baseUrl());
            error.setLine(import->location.line);
            error.setColumn(import->location.column);
            errors.prepend(error); // put it back on the list after filling out information.
            setError(errors);
            return;
        }
    }
}

void QQmlTypeData::allDependenciesDone()
{
    QQmlTypeLoader::Blob::allDependenciesDone();

    if (!m_typesResolved) {
        // Check that all imports were resolved
        QList<QQmlError> errors;
        auto it = m_unresolvedImports.constBegin(), end = m_unresolvedImports.constEnd();
        for ( ; it != end; ++it) {
            if ((*it)->priority == 0) {
                // This import was not resolved
                for (auto keyIt = m_unresolvedImports.constBegin(),
                          keyEnd = m_unresolvedImports.constEnd();
                     keyIt != keyEnd; ++keyIt) {
                    PendingImportPtr import = *keyIt;
                    QQmlError error;
                    error.setDescription(QQmlTypeLoader::tr("module \"%1\" is not installed").arg(import->uri));
                    error.setUrl(m_importCache.baseUrl());
                    error.setLine(import->location.line);
                    error.setColumn(import->location.column);
                    errors.prepend(error);
                }
            }
        }
        if (errors.size()) {
            setError(errors);
            return;
        }

        resolveTypes();
        m_typesResolved = true;
    }
}

void QQmlTypeData::downloadProgressChanged(qreal p)
{
    for (int ii = 0; ii < m_callbacks.count(); ++ii) {
        TypeDataCallback *callback = m_callbacks.at(ii);
        callback->typeDataProgress(this, p);
    }
}

QString QQmlTypeData::stringAt(int index) const
{
    if (m_compiledData)
        return m_compiledData->stringAt(index);
    return m_document->jsGenerator.stringTable.stringForIndex(index);
}

void QQmlTypeData::compile(const QQmlRefPointer<QQmlTypeNameCache> &typeNameCache,
                           QV4::ResolvedTypeReferenceMap *resolvedTypeCache,
                           const QV4::CompiledData::DependentTypesHasher &dependencyHasher)
{
    Q_ASSERT(m_compiledData.isNull());

    const bool typeRecompilation = m_document && m_document->javaScriptCompilationUnit.unitData()
            && (m_document->javaScriptCompilationUnit.unitData()->flags & QV4::CompiledData::Unit::PendingTypeCompilation);

    QQmlEnginePrivate * const enginePrivate = QQmlEnginePrivate::get(typeLoader()->engine());
    QQmlTypeCompiler compiler(enginePrivate, this, m_document.data(), typeNameCache, resolvedTypeCache, dependencyHasher);
    m_compiledData = compiler.compile();
    if (!m_compiledData) {
        setError(compiler.compilationErrors());
        return;
    }

    const bool trySaveToDisk = (!diskCacheDisabled() || diskCacheForced())
            && !m_document->jsModule.debugMode && !typeRecompilation;
    if (trySaveToDisk) {
        QString errorString;
        if (m_compiledData->saveToDisk(url(), &errorString)) {
            QString error;
            if (!m_compiledData->loadFromDisk(url(), m_backupSourceCode.sourceTimeStamp(), &error)) {
                // ignore error, keep using the in-memory compilation unit.
            }
        } else {
            qCDebug(DBG_DISK_CACHE) << "Error saving cached version of" << m_compiledData->fileName() << "to disk:" << errorString;
        }
    }
}

void QQmlTypeData::resolveTypes()
{
    // Add any imported scripts to our resolved set
    const auto resolvedScripts = m_importCache.resolvedScripts();
    for (const QQmlImports::ScriptReference &script : resolvedScripts) {
        QQmlRefPointer<QQmlScriptBlob> blob = typeLoader()->getScript(script.location);
        addDependency(blob.data());

        ScriptReference ref;
        //ref.location = ...
        if (!script.qualifier.isEmpty())
        {
            ref.qualifier = script.qualifier + QLatin1Char('.') + script.nameSpace;
            // Add a reference to the enclosing namespace
            m_namespaces.insert(script.qualifier);
        } else {
            ref.qualifier = script.nameSpace;
        }

        ref.script = blob;
        m_scripts << ref;
    }

    // Lets handle resolved composite singleton types
    const auto resolvedCompositeSingletons = m_importCache.resolvedCompositeSingletons();
    for (const QQmlImports::CompositeSingletonReference &csRef : resolvedCompositeSingletons) {
        TypeReference ref;
        QString typeName;
        if (!csRef.prefix.isEmpty()) {
            typeName = csRef.prefix + QLatin1Char('.') + csRef.typeName;
            // Add a reference to the enclosing namespace
            m_namespaces.insert(csRef.prefix);
        } else {
            typeName = csRef.typeName;
        }

        int majorVersion = csRef.majorVersion > -1 ? csRef.majorVersion : -1;
        int minorVersion = csRef.minorVersion > -1 ? csRef.minorVersion : -1;

        if (!resolveType(typeName, majorVersion, minorVersion, ref, -1, -1, true,
                         QQmlType::CompositeSingletonType))
            return;

        if (ref.type.isCompositeSingleton()) {
            ref.typeData = typeLoader()->getType(ref.type.sourceUrl());
            if (ref.typeData->status() == QQmlDataBlob::ResolvingDependencies) {
                // TODO: give an error message? If so, we should record and show the path of the cycle.
                continue;
            }
            addDependency(ref.typeData.data());
            ref.prefix = csRef.prefix;

            m_compositeSingletons << ref;
        }
    }

    for (QV4::CompiledData::TypeReferenceMap::ConstIterator unresolvedRef = m_typeReferences.constBegin(), end = m_typeReferences.constEnd();
         unresolvedRef != end; ++unresolvedRef) {

        TypeReference ref; // resolved reference

        const bool reportErrors = unresolvedRef->errorWhenNotFound;

        int majorVersion = -1;
        int minorVersion = -1;

        const QString name = stringAt(unresolvedRef.key());

        if (!resolveType(name, majorVersion, minorVersion, ref, unresolvedRef->location.line,
                         unresolvedRef->location.column, reportErrors,
                         QQmlType::AnyRegistrationType) && reportErrors)
            return;

        if (ref.type.isComposite()) {
            ref.typeData = typeLoader()->getType(ref.type.sourceUrl());
            addDependency(ref.typeData.data());
        }
        ref.majorVersion = majorVersion;
        ref.minorVersion = minorVersion;

        ref.location.line = unresolvedRef->location.line;
        ref.location.column = unresolvedRef->location.column;

        ref.needsCreation = unresolvedRef->needsCreation;

        m_resolvedTypes.insert(unresolvedRef.key(), ref);
    }

    // ### this allows enums to work without explicit import or instantiation of the type
    if (!m_implicitImportLoaded)
        loadImplicitImport();
}

QQmlJS::DiagnosticMessage QQmlTypeData::buildTypeResolutionCaches(
        QQmlRefPointer<QQmlTypeNameCache> *typeNameCache,
        QV4::ResolvedTypeReferenceMap *resolvedTypeCache
        ) const
{
    typeNameCache->adopt(new QQmlTypeNameCache(m_importCache));

    for (const QString &ns: m_namespaces)
        (*typeNameCache)->add(ns);

    // Add any Composite Singletons that were used to the import cache
    for (const QQmlTypeData::TypeReference &singleton: m_compositeSingletons)
        (*typeNameCache)->add(singleton.type.qmlTypeName(), singleton.type.sourceUrl(), singleton.prefix);

    m_importCache.populateCache(typeNameCache->data());

    QQmlEnginePrivate * const engine = QQmlEnginePrivate::get(typeLoader()->engine());

    for (auto resolvedType = m_resolvedTypes.constBegin(), end = m_resolvedTypes.constEnd(); resolvedType != end; ++resolvedType) {
        QScopedPointer<QV4::ResolvedTypeReference> ref(new QV4::ResolvedTypeReference);
        QQmlType qmlType = resolvedType->type;
        if (resolvedType->typeData) {
            if (resolvedType->needsCreation && qmlType.isCompositeSingleton()) {
                return qQmlCompileError(resolvedType->location, tr("Composite Singleton Type %1 is not creatable.").arg(qmlType.qmlTypeName()));
            }
            ref->compilationUnit = resolvedType->typeData->compilationUnit();
        } else if (qmlType.isValid()) {
            ref->type = qmlType;
            Q_ASSERT(ref->type.isValid());

            if (resolvedType->needsCreation && !ref->type.isCreatable()) {
                QString reason = ref->type.noCreationReason();
                if (reason.isEmpty())
                    reason = tr("Element is not creatable.");
                return qQmlCompileError(resolvedType->location, reason);
            }

            if (ref->type.containsRevisionedAttributes()) {
                ref->typePropertyCache = engine->cache(ref->type,
                                                       resolvedType->minorVersion);
            }
        }
        ref->majorVersion = resolvedType->majorVersion;
        ref->minorVersion = resolvedType->minorVersion;
        ref->doDynamicTypeCheck();
        resolvedTypeCache->insert(resolvedType.key(), ref.take());
    }
    QQmlJS::DiagnosticMessage noError;
    return noError;
}

bool QQmlTypeData::resolveType(const QString &typeName, int &majorVersion, int &minorVersion,
                               TypeReference &ref, int lineNumber, int columnNumber,
                               bool reportErrors, QQmlType::RegistrationType registrationType)
{
    QQmlImportNamespace *typeNamespace = nullptr;
    QList<QQmlError> errors;

    bool typeFound = m_importCache.resolveType(typeName, &ref.type, &majorVersion, &minorVersion,
                                               &typeNamespace, &errors, registrationType);
    if (!typeNamespace && !typeFound && !m_implicitImportLoaded) {
        // Lazy loading of implicit import
        if (loadImplicitImport()) {
            // Try again to find the type
            errors.clear();
            typeFound = m_importCache.resolveType(typeName, &ref.type, &majorVersion, &minorVersion,
                                                  &typeNamespace, &errors, registrationType);
        } else {
            return false; //loadImplicitImport() hit an error, and called setError already
        }
    }

    if ((!typeFound || typeNamespace) && reportErrors) {
        // Known to not be a type:
        //  - known to be a namespace (Namespace {})
        //  - type with unknown namespace (UnknownNamespace.SomeType {})
        QQmlError error;
        if (typeNamespace) {
            error.setDescription(QQmlTypeLoader::tr("Namespace %1 cannot be used as a type").arg(typeName));
        } else {
            if (errors.size()) {
                error = errors.takeFirst();
            } else {
                // this should not be possible!
                // Description should come from error provided by addImport() function.
                error.setDescription(QQmlTypeLoader::tr("Unreported error adding script import to import database"));
            }
            error.setUrl(m_importCache.baseUrl());
            error.setDescription(QQmlTypeLoader::tr("%1 %2").arg(typeName).arg(error.description()));
        }

        if (lineNumber != -1)
            error.setLine(lineNumber);
        if (columnNumber != -1)
            error.setColumn(columnNumber);

        errors.prepend(error);
        setError(errors);
        return false;
    }

    return true;
}

void QQmlTypeData::scriptImported(const QQmlRefPointer<QQmlScriptBlob> &blob, const QV4::CompiledData::Location &location, const QString &qualifier, const QString &/*nameSpace*/)
{
    ScriptReference ref;
    ref.script = blob;
    ref.location = location;
    ref.qualifier = qualifier;

    m_scripts << ref;
}

QT_END_NAMESPACE
