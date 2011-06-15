/**************************************************************************
**
** This file is part of Qt SDK**
**
** Copyright (c) 2011 Nokia Corporation and/or its subsidiary(-ies).*
**
** Contact:  Nokia Corporation qt-info@nokia.com**
**
** No Commercial Usage
**
** This file contains pre-release code and may not be distributed.
** You may use this file in accordance with the terms and conditions
** contained in the Technology Preview License Agreement accompanying
** this package.
**
** GNU Lesser General Public License Usage
**
** This file may be used under the terms of the GNU Lesser General Public
** License version 2.1 as published by the Free Software Foundation and
** appearing in the file LICENSE.LGPL included in the packaging of this file.
** Please review the following information to ensure the GNU Lesser General
** Public License version 2.1 requirements will be met:
** http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** In addition, as a special exception, Nokia gives you certain additional
** rights. These rights are described in the Nokia Qt LGPL Exception version
** 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
** If you are unsure which license is appropriate for your use, please contact
** (qt-info@nokia.com).
**
**************************************************************************/
#include "packagemanagercore.h"

#include "adminauthorization.h"
#include "common/binaryformat.h"
#include "common/errors.h"
#include "common/installersettings.h"
#include "common/utils.h"
#include "component.h"
#include "downloadarchivesjob.h"
#include "fsengineclient.h"
#include "getrepositoriesmetainfojob.h"
#include "messageboxhandler.h"
#include "packagemanagercore_p.h"
#include "progresscoordinator.h"
#include "qinstallerglobal.h"
#include "qprocesswrapper.h"
#include "qsettingswrapper.h"

#include <QtCore/QTemporaryFile>

#include <QtGui/QDesktopServices>

#include <QtScript/QScriptEngine>
#include <QtScript/QScriptContext>

#include <KDToolsCore/KDSysInfo>

#ifdef Q_OS_WIN
#include "qt_windows.h"
#endif

using namespace QInstaller;

static QFont sVirtualComponentsFont;

static bool sNoForceInstallation = false;
static bool sVirtualComponentsVisible = false;

static QScriptValue checkArguments(QScriptContext* context, int amin, int amax)
{
    if (context->argumentCount() < amin || context->argumentCount() > amax) {
        if (amin != amax) {
            return context->throwError(QObject::tr("Invalid arguments: %1 arguments given, %2 to "
                "%3 expected.").arg(QString::number(context->argumentCount()),
                QString::number(amin), QString::number(amax)));
        }
        return context->throwError(QObject::tr("Invalid arguments: %1 arguments given, %2 expected.")
            .arg(QString::number(context->argumentCount()), QString::number(amin)));
    }
    return QScriptValue();
}

/*!
    Appends \a comp preceded by its dependencies to \a components. Makes sure components contains
    every component only once.
    \internal
*/
static void appendComponentAndMissingDependencies(QList<Component*>& components, Component* comp)
{
    if (comp == 0)
        return;

    const QList<Component*> deps = comp->packageManagerCore()->missingDependencies(comp);
    foreach (Component *component, deps)
        appendComponentAndMissingDependencies(components, component);

    if (!components.contains(comp))
        components.push_back(comp);
}

static bool componentMatches(const Component *component, const QString &name,
    const QString& version = QString())
{
    if (name.isEmpty() || component->name() != name)
        return false;

    if (version.isEmpty())
        return true;

    return PackageManagerCore::versionMatches(component->value(scVersion), version);
}

Component* PackageManagerCore::subComponentByName(const QInstaller::PackageManagerCore *installer, const QString &name,
    const QString &version, Component *check)
{
    if (name.isEmpty())
        return 0;

    if (check != 0 && componentMatches(check, name, version))
        return check;

    if (installer->runMode() == AllMode) {
        const QList<Component*> rootComponents = check == 0 ? installer->components(false, AllMode)
            : check->childComponents(false, AllMode);
        foreach (QInstaller::Component* component, rootComponents) {
            Component* const result = subComponentByName(installer, name, version, component);
            if (result != 0)
                return result;
        }
    } else {
        const QList<Component*> updaterComponents = installer->components(false, UpdaterMode)
            + installer->d->m_updaterComponentsDeps;
        foreach (QInstaller::Component *component, updaterComponents) {
            if (componentMatches(component, name, version))
                return component;
        }
    }
    return 0;
}

/*!
    Scriptable version of PackageManagerCore::componentByName(QString).
    \sa PackageManagerCore::componentByName
 */
QScriptValue QInstaller::qInstallerComponentByName(QScriptContext* context, QScriptEngine* engine)
{
    const QScriptValue check = checkArguments(context, 1, 1);
    if (check.isError())
        return check;

    // well... this is our "this" pointer
    PackageManagerCore *const core = dynamic_cast<PackageManagerCore*>(engine->globalObject()
        .property(QLatin1String("installer")).toQObject());

    const QString name = context->argument(0).toString();
    return engine->newQObject(core->componentByName(name));
}

QScriptValue QInstaller::qDesktopServicesOpenUrl(QScriptContext* context, QScriptEngine* engine)
{
    Q_UNUSED(engine);
    const QScriptValue check = checkArguments(context, 1, 1);
    if (check.isError())
        return check;
    QString url = context->argument(0).toString();
    url.replace(QLatin1String("\\\\"), QLatin1String("/"));
    url.replace(QLatin1String("\\"), QLatin1String("/"));
    return QDesktopServices::openUrl(QUrl::fromUserInput(url));
}

QScriptValue QInstaller::qDesktopServicesDisplayName(QScriptContext* context, QScriptEngine* engine)
{
    Q_UNUSED(engine);
    const QScriptValue check = checkArguments(context, 1, 1);
    if (check.isError())
        return check;
    const QDesktopServices::StandardLocation location =
        static_cast< QDesktopServices::StandardLocation >(context->argument(0).toInt32());
    return QDesktopServices::displayName(location);
}

QScriptValue QInstaller::qDesktopServicesStorageLocation(QScriptContext* context, QScriptEngine* engine)
{
    Q_UNUSED(engine);
    const QScriptValue check = checkArguments(context, 1, 1);
    if (check.isError())
        return check;
    const QDesktopServices::StandardLocation location =
        static_cast< QDesktopServices::StandardLocation >(context->argument(0).toInt32());
    return QDesktopServices::storageLocation(location);
}

QString QInstaller::uncaughtExceptionString(QScriptEngine *scriptEngine/*, const QString &context*/)
{
    //QString errorString(QLatin1String("%1 %2\n%3"));
    QString errorString(QLatin1String("\t\t%1\n%2"));
    //if (!context.isEmpty())
    //    errorString.prepend(context + QLatin1String(": "));

    //usually the line number is in the backtrace
    errorString = errorString.arg(/*QString::number(scriptEngine->uncaughtExceptionLineNumber()),*/
        scriptEngine->uncaughtException().toString(), scriptEngine->uncaughtExceptionBacktrace()
        .join(QLatin1String("\n")));
    return errorString;
}


/*!
    \class QInstaller::PackageManagerCore
    PackageManagerCore forms the core of the installation and uninstallation system.
 */

/*!
    \enum QInstaller::PackageManagerCore::WizardPage
    WizardPage is used to number the different pages known to the Installer GUI.
 */

/*!
    \var QInstaller::PackageManagerCore::Introduction
  I ntroduction page.
 */

/*!
    \var QInstaller::PackageManagerCore::LicenseCheck
    License check page
 */
/*!
    \var QInstaller::PackageManagerCore::TargetDirectory
    Target directory selection page
 */
/*!
    \var QInstaller::PackageManagerCore::ComponentSelection
    %Component selection page
 */
/*!
    \var QInstaller::PackageManagerCore::StartMenuSelection
    Start menu directory selection page - Microsoft Windows only
 */
/*!
    \var QInstaller::PackageManagerCore::ReadyForInstallation
    "Ready for Installation" page
 */
/*!
    \var QInstaller::PackageManagerCore::PerformInstallation
    Page shown while performing the installation
 */
/*!
    \var QInstaller::PackageManagerCore::InstallationFinished
    Page shown when the installation was finished
 */
/*!
    \var QInstaller::PackageManagerCore::End
    Non-existing page - this value has to be used if you want to insert a page after \a InstallationFinished
 */

KDUpdater::Application& PackageManagerCore::updaterApplication() const
{
    return *d->m_app;
}

void PackageManagerCore::setUpdaterApplication(KDUpdater::Application *app)
{
    d->m_app = app;
}

void PackageManagerCore::writeUninstaller()
{
    if (d->m_needToWriteUninstaller) {
        try {
            d->writeUninstaller(d->m_performedOperationsOld + d->m_performedOperationsCurrentSession);

            bool gainedAdminRights = false;
            QTemporaryFile tempAdminFile(d->targetDir()
                + QLatin1String("/testjsfdjlkdsjflkdsjfldsjlfds") + QString::number(qrand() % 1000));
            if (!tempAdminFile.open() || !tempAdminFile.isWritable()) {
                gainAdminRights();
                gainedAdminRights = true;
            }
            d->m_app->packagesInfo()->writeToDisk();
            if (gainedAdminRights)
                dropAdminRights();
            d->m_needToWriteUninstaller = false;
        } catch (const Error &error) {
            MessageBoxHandler::critical(MessageBoxHandler::currentBestSuitParent(),
                QLatin1String("WriteError"), tr("Error writing Uninstaller"), error.message(),
                QMessageBox::Ok, QMessageBox::Ok);
        }
    }
}

void PackageManagerCore::reset(const QHash<QString, QString> &params)
{
    d->m_completeUninstall = false;
    d->m_forceRestart = false;
    d->m_status = PackageManagerCore::Unfinished;
    d->m_installerBaseBinaryUnreplaced.clear();
    d->m_vars.clear();
    d->m_vars = params;
    d->initialize();
}

/*!
    Sets the uninstallation to be \a complete. If \a complete is false, only components deselected
    by the user will be uninstalled. This option applies only on uninstallation.
 */
void PackageManagerCore::setCompleteUninstallation(bool complete)
{
    d->m_completeUninstall = complete;
}

void PackageManagerCore::autoAcceptMessageBoxes()
{
    MessageBoxHandler::instance()->setDefaultAction(MessageBoxHandler::Accept);
}

void PackageManagerCore::autoRejectMessageBoxes()
{
    MessageBoxHandler::instance()->setDefaultAction(MessageBoxHandler::Reject);
}

void PackageManagerCore::setMessageBoxAutomaticAnswer(const QString &identifier, int button)
{
    MessageBoxHandler::instance()->setAutomaticAnswer(identifier,
        static_cast<QMessageBox::Button>(button));
}

// TODO: figure out why we have this function at all
void PackageManagerCore::installSelectedComponents()
{
    d->setStatus(PackageManagerCore::Running);
    // download

    double downloadPartProgressSize = double(1)/3;
    double componentsInstallPartProgressSize = double(2)/3;
    // get the list of packages we need to install in proper order and do it for the updater

    // TODO: why only updater mode???
    const int downloadedArchivesCount = downloadNeededArchives(UpdaterMode, downloadPartProgressSize);

    //if there was no download we have the whole progress for installing components
    if (!downloadedArchivesCount) {
         //componentsInstallPartProgressSize + downloadPartProgressSize;
        componentsInstallPartProgressSize = double(1);
    }

    // get the list of packages we need to install in proper order
    const QList<Component*> components = componentsToInstall(runMode());

    if (!isInstaller() && !QFileInfo(installerBinaryPath()).isWritable())
        gainAdminRights();

    d->stopProcessesForUpdates(components);
    int progressOperationCount = d->countProgressOperations(components);
    double progressOperationSize = componentsInstallPartProgressSize / progressOperationCount;

    // TODO: divide this in undo steps and install steps (2 "for" loops) for better progress calculation
    foreach (Component* const currentComponent, components) {
        if (d->statusCanceledOrFailed())
            throw Error(tr("Installation canceled by user"));
        ProgressCoordninator::instance()->emitLabelAndDetailTextChanged(tr("\nRemoving the old "
            "version of: %1").arg(currentComponent->name()));
        if ((isUpdater() || isPackageManager()) && currentComponent->removeBeforeUpdate()) {
            QString replacesAsString = currentComponent->value(scReplaces);
            QStringList possibleNames(replacesAsString.split(QLatin1Char(','), QString::SkipEmptyParts));
            possibleNames.append(currentComponent->name());

            // undo all operations done by this component upon installation
            for (int i = d->m_performedOperationsOld.count() - 1; i >= 0; --i) {
                KDUpdater::UpdateOperation* const op = d->m_performedOperationsOld.at(i);
                if (!possibleNames.contains(op->value(QLatin1String("component")).toString()))
                    continue;
                const bool becameAdmin = !d->m_FSEngineClientHandler->isActive()
                    && op->value(QLatin1String("admin")).toBool() && gainAdminRights();
                PackageManagerCorePrivate::performOperationThreaded(op, PackageManagerCorePrivate::Undo);
                if (becameAdmin)
                    dropAdminRights();
                delete d->m_performedOperationsOld.takeAt(i);
            }
            foreach(const QString possilbeName, possibleNames)
                d->m_app->packagesInfo()->removePackage(possilbeName);
            d->m_app->packagesInfo()->writeToDisk();
        }
        ProgressCoordninator::instance()->emitLabelAndDetailTextChanged(
            tr("\nInstalling the new version of: %1").arg(currentComponent->name()));
        installComponent(currentComponent, progressOperationSize);
        //commit all operations for this already updated/installed component
        //so an undo during the installComponent function only undoes the uncompleted installed one
        d->commitSessionOperations();
        d->m_needToWriteUninstaller = true;
    }

    d->setStatus(PackageManagerCore::Success);
    ProgressCoordninator::instance()->emitLabelAndDetailTextChanged(tr("\nUpdate finished!"));
    emit updateFinished();
}

quint64 size(QInstaller::Component *component, const QString &value)
{
    if (!component->isSelected() || component->isInstalled())
        return quint64(0);
    return component->value(value).toLongLong();
}

quint64 PackageManagerCore::requiredDiskSpace() const
{
    quint64 result = 0;

    const QList<Component*> availableComponents = components(true, runMode());
    foreach (QInstaller::Component *component, availableComponents)
        result += size(component, scUncompressedSize);

    return result;
}

quint64 PackageManagerCore::requiredTemporaryDiskSpace() const
{
    quint64 result = 0;

    const QList<Component*> availableComponents = components(true, runMode());
    foreach (QInstaller::Component *component, availableComponents)
        result += size(component, scCompressedSize);

    return result;
}

/*!
    Returns the will be downloaded archives count
*/
int PackageManagerCore::downloadNeededArchives(RunMode runMode, double partProgressSize)
{
    Q_ASSERT(partProgressSize >= 0 && partProgressSize <= 1);

    QList<QPair<QString, QString> > archivesToDownload;
    QList<Component*> neededComponents = componentsToInstall(runMode);
    foreach (Component *component, neededComponents) {
        // collect all archives to be downloaded
        const QStringList toDownload = component->downloadableArchives();
        foreach (const QString &versionFreeString, toDownload) {
            archivesToDownload.push_back(qMakePair(QString::fromLatin1("installer://%1/%2")
                .arg(component->name(), versionFreeString), QString::fromLatin1("%1/%2/%3")
                .arg(component->repositoryUrl().toString(), component->name(), versionFreeString)));
        }
    }

    if (archivesToDownload.isEmpty())
        return 0;

    ProgressCoordninator::instance()->emitLabelAndDetailTextChanged(tr("\nDownloading packages..."));

    // don't have it on the stack, since it keeps the temporary files
    DownloadArchivesJob* const archivesJob =
        new DownloadArchivesJob(d->m_installerSettings.publicKey(), this);
    archivesJob->setAutoDelete(false);
    archivesJob->setArchivesToDownload(archivesToDownload);
    connect(this, SIGNAL(installationInterrupted()), archivesJob, SLOT(cancel()));
    connect(archivesJob, SIGNAL(outputTextChanged(QString)), ProgressCoordninator::instance(),
        SLOT(emitLabelAndDetailTextChanged(QString)));

    ProgressCoordninator::instance()->registerPartProgress(archivesJob, SIGNAL(progressChanged(double)),
        partProgressSize);

    archivesJob->start();
    archivesJob->waitForFinished();

    if (archivesJob->error() == KDJob::Canceled)
        interrupt();
    else if (archivesJob->error() != KDJob::NoError)
        throw Error(archivesJob->errorString());

    if (d->statusCanceledOrFailed())
        throw Error(tr("Installation canceled by user"));

    return archivesToDownload.count();
}

void PackageManagerCore::installComponent(Component *component, double progressOperationSize)
{
    Q_ASSERT(progressOperationSize);

    d->setStatus(PackageManagerCore::Running);
    try {
        d->installComponent(component, progressOperationSize);
        d->setStatus(PackageManagerCore::Success);
    } catch (const Error &error) {
        if (status() != PackageManagerCore::Canceled) {
            d->setStatus(PackageManagerCore::Failure);
            verbose() << "INSTALLER FAILED: " << error.message() << std::endl;
            MessageBoxHandler::critical(MessageBoxHandler::currentBestSuitParent(),
                QLatin1String("installationError"), tr("Error"), error.message());
        }
    }
}

/*!
    If a component marked as important was installed during update
    process true is returned.
*/
bool PackageManagerCore::needsRestart() const
{
    return d->m_forceRestart;
}

void PackageManagerCore::rollBackInstallation()
{
    emit titleMessageChanged(tr("Cancelling the Installer"));

    KDUpdater::PackagesInfo *const packages = d->m_app->packagesInfo();
    packages->setFileName(d->componentsXmlPath()); // forces a refresh of installed packages
    packages->setApplicationName(d->m_installerSettings.applicationName());
    packages->setApplicationVersion(d->m_installerSettings.applicationVersion());

    //this unregisters all operation progressChanged connects
    ProgressCoordninator::instance()->setUndoMode();
    const int progressOperationCount = d->countProgressOperations(d->m_performedOperationsCurrentSession);
    const double progressOperationSize = double(1) / progressOperationCount;

    //re register all the undo operations with the new size to the ProgressCoordninator
    foreach (KDUpdater::UpdateOperation* const operation, d->m_performedOperationsCurrentSession) {
        QObject* const operationObject = dynamic_cast<QObject*>(operation);
        if (operationObject != 0) {
            const QMetaObject* const mo = operationObject->metaObject();
            if (mo->indexOfSignal(QMetaObject::normalizedSignature("progressChanged(double)")) > -1) {
                ProgressCoordninator::instance()->registerPartProgress(operationObject,
                    SIGNAL(progressChanged(double)), progressOperationSize);
            }
        }
    }

    while (!d->m_performedOperationsCurrentSession.isEmpty()) {
        try {
            KDUpdater::UpdateOperation *const operation = d->m_performedOperationsCurrentSession.takeLast();
            const bool becameAdmin = !d->m_FSEngineClientHandler->isActive()
                && operation->value(QLatin1String("admin")).toBool() && gainAdminRights();

            PackageManagerCorePrivate::performOperationThreaded(operation, PackageManagerCorePrivate::Undo);

            const QString componentName = operation->value(QLatin1String("component")).toString();
            if (!componentName.isEmpty()) {
                Component *component = componentByName(componentName);
                if (!component)
                    component = d->componentsToReplace().value(componentName).second;
                if (component) {
                    component->setUninstalled();
                    packages->removePackage(component->name());
                }
            }

            if (becameAdmin)
                dropAdminRights();
        } catch (const Error &e) {
            MessageBoxHandler::critical(MessageBoxHandler::currentBestSuitParent(),
                QLatin1String("ElevationError"), tr("Authentication Error"), tr("Some components "
                "could not be removed completely because admin rights could not be acquired: %1.")
                .arg(e.message()));
        } catch (...) {
            MessageBoxHandler::critical(MessageBoxHandler::currentBestSuitParent(), QLatin1String("unknown"),
                tr("Unknown error."), tr("Some components could not be removed completely because an unknown "
                "error happend."));
        }
    }
    packages->writeToDisk();
}

bool PackageManagerCore::isFileExtensionRegistered(const QString& extension) const
{
    QSettingsWrapper settings(QLatin1String("HKEY_CLASSES_ROOT"), QSettingsWrapper::NativeFormat);
    return settings.value(QString::fromLatin1(".%1/Default").arg(extension)).isValid();
}


// -- QInstaller

/*!
    Used by operation runner to get a fake installer, can be removed if installerbase can do what operation
    runner does.
*/
PackageManagerCore::PackageManagerCore()
    : d(new PackageManagerCorePrivate(this))
{
}

PackageManagerCore::PackageManagerCore(qint64 magicmaker,
        const QVector<KDUpdater::UpdateOperation*>& performedOperations)
    : d(new PackageManagerCorePrivate(this, magicmaker, performedOperations.toList()))
{
    qRegisterMetaType<QInstaller::PackageManagerCore::Status>("QInstaller::PackageManagerCore::Status");
    qRegisterMetaType<QInstaller::PackageManagerCore::WizardPage>("QInstaller::PackageManagerCore::WizardPage");

    d->initialize();
}

PackageManagerCore::~PackageManagerCore()
{
    if (!isUninstaller() && !(isInstaller() && status() == PackageManagerCore::Canceled)) {
        QDir targetDir(value(scTargetDir));
        QString logFileName = targetDir.absoluteFilePath(value(QLatin1String("LogFileName"),
            QLatin1String("InstallationLog.txt")));
        QInstaller::VerboseWriter::instance()->setOutputStream(logFileName);
    }
    delete d;
}

/* static */
QFont PackageManagerCore::virtualComponentsFont()
{
    return sVirtualComponentsFont;
}

/* static */
void PackageManagerCore::setVirtualComponentsFont(const QFont &font)
{
    sVirtualComponentsFont = font;
}

/* static */
bool PackageManagerCore::virtualComponentsVisible()
{
    return sVirtualComponentsVisible;
}

/* static */
void PackageManagerCore::setVirtualComponentsVisible(bool visible)
{
    sVirtualComponentsVisible = visible;
}

/* static */
bool PackageManagerCore::noForceInstallation()
{
    return sNoForceInstallation;
}

/* static */
void PackageManagerCore::setNoForceInstallation(bool value)
{
    sNoForceInstallation = value;
}

RunMode PackageManagerCore::runMode() const
{
    return isUpdater() ? UpdaterMode : AllMode;
}

/*!
    Returns a hash containing the installed package name and it's associated package information. If
    the application is running in installer mode or the local components file could not be parsed, the
    hash is empty.
*/
QHash<QString, KDUpdater::PackageInfo> PackageManagerCore::localInstalledPackages()
{
    QHash<QString, KDUpdater::PackageInfo> installedPackages;

    if (!isInstaller()) {
        KDUpdater::PackagesInfo &packagesInfo = *d->m_app->packagesInfo();
        if (!setAndParseLocalComponentsFile(packagesInfo)) {
            verbose() << tr("Could not parse local components xml file: %1")
                .arg(d->localComponentsXmlPath());
            return installedPackages;
        }
        packagesInfo.setApplicationName(d->m_installerSettings.applicationName());
        packagesInfo.setApplicationVersion(d->m_installerSettings.applicationVersion());

        foreach (const KDUpdater::PackageInfo &info, packagesInfo.packageInfos())
            installedPackages.insert(info.name, info);
     }

    return installedPackages;
}

GetRepositoriesMetaInfoJob* PackageManagerCore::fetchMetaInformation(const QInstaller::InstallerSettings &settings)
{
    GetRepositoriesMetaInfoJob *metaInfoJob = new GetRepositoriesMetaInfoJob(settings.publicKey());
    if ((isInstaller() && !isOfflineOnly()) || (isUpdater() || isPackageManager()))
        metaInfoJob->setRepositories(settings.repositories());

    connect(metaInfoJob, SIGNAL(infoMessage(KDJob*, QString)), this,
        SIGNAL(metaJobInfoMessage(KDJob*, QString)));
    connect(this, SIGNAL(cancelMetaInfoJob()), metaInfoJob, SLOT(doCancel()));

    try {
        metaInfoJob->setAutoDelete(false);
        metaInfoJob->start();
        metaInfoJob->waitForFinished();
    } catch (Error &error) {
        verbose() << tr("Could not retrieve meta information: %1").arg(error.message()) << std::endl;
    }

    return metaInfoJob;
}

bool PackageManagerCore::addUpdateResourcesFrom(GetRepositoriesMetaInfoJob *metaInfoJob, const InstallerSettings &settings,
    bool parseChecksum)
{
    const QString &appName = settings.applicationName();
    const QStringList tempDirs = metaInfoJob->temporaryDirectories();
    foreach (const QString &tmpDir, tempDirs) {
        if (tmpDir.isEmpty())
            continue;

        if (parseChecksum) {
            const QString updatesXmlPath = tmpDir + QLatin1String("/Updates.xml");
            QFile updatesFile(updatesXmlPath);
            try {
                openForRead(&updatesFile, updatesFile.fileName());
            } catch(const Error &e) {
                verbose() << tr("Error opening Updates.xml: ") << e.message() << std::endl;
                return false;
            }

            int line = 0;
            int column = 0;
            QString error;
            QDomDocument doc;
            if (!doc.setContent(&updatesFile, &error, &line, &column)) {
                verbose() << tr("Parse error in File %4 : %1 at line %2 col %3").arg(error,
                    QString::number(line), QString::number(column), updatesFile.fileName()) << std::endl;
                return false;
            }

            const QDomNode checksum = doc.documentElement().firstChildElement(QLatin1String("Checksum"));
            if (!checksum.isNull())
                setTestChecksum(checksum.toElement().text().toLower() == scTrue);
        }
        d->m_app->addUpdateSource(appName, appName, QString(), QUrl::fromLocalFile(tmpDir), 1);
    }
    d->m_app->updateSourcesInfo()->setModified(false);

    return true;
}

bool PackageManagerCore::fetchAllPackages()
{
    if (isUninstaller() || isUpdater())
        return false;

    QHash<QString, KDUpdater::PackageInfo> installedPackages = localInstalledPackages();

    QScopedPointer <GetRepositoriesMetaInfoJob> metaInfoJob(fetchMetaInformation(d->m_installerSettings));
    if (metaInfoJob->isCanceled() || metaInfoJob->error() != KDJob::NoError) {
        if (metaInfoJob->error() != QInstaller::UserIgnoreError) {
            verbose() << tr("Could not retrieve updates: %1").arg(metaInfoJob->errorString()) << std::endl;
            return false;
        }
    }

    if (!metaInfoJob->temporaryDirectories().isEmpty()) {
        if (!addUpdateResourcesFrom(metaInfoJob.data(), d->m_installerSettings, true)) {
            verbose() << tr("Could not add temporary update source information.") << std::endl;
            return false;
        }
    }

    if (d->m_app->updateSourcesInfo()->updateSourceInfoCount() == 0) {
        verbose() << tr("Could not find any update source information.") << std::endl;
        return false;
    }

    KDUpdater::UpdateFinder updateFinder(d->m_app);
    updateFinder.setAutoDelete(false);
    updateFinder.setUpdateType(KDUpdater::PackageUpdate | KDUpdater::NewPackage);
    updateFinder.run();

    const QList<KDUpdater::Update*> &packages = updateFinder.updates();
    if (packages.isEmpty()) {
        verbose() << tr("Could not retrieve components: %1").arg(updateFinder.errorString());
        return false;
    }

    emit startAllComponentsReset();

    d->clearAllComponentLists();
    QMap<QString, QInstaller::Component*> components;

    Data data;
    data.components = &components;
    data.metaInfoJob = metaInfoJob.data();
    data.installedPackages = &installedPackages;

    foreach (KDUpdater::Update *package, packages) {
        QScopedPointer<QInstaller::Component> component(new QInstaller::Component(this));

        data.package = package;
        component->loadDataFromUpdate(package);
        if (updateComponentData(data, component.data())) {
            const QString name = component->name();
            components.insert(name, component.take());
        }
    }

    // store all components that got a replacement
    storeReplacedComponents(components, data.replacementToExchangeables);

    // now append all components to their respective parents
    QMap<QString, QInstaller::Component*>::const_iterator it;
    for (it = components.begin(); it != components.end(); ++it) {
        QString id = it.key();
        QInstaller::Component *component = it.value();
        while (!id.isEmpty() && component->parentComponent() == 0) {
            id = id.section(QLatin1Char('.'), 0, -2);
            if (components.contains(id))
                components[id]->appendComponent(component);
        }
    }

    // append all components w/o parent to the direct list
    foreach (QInstaller::Component *component, components) {
        if (component->parentComponent() == 0)
            appendRootComponent(component, AllMode);
    }

    // after everything is set up, load the scripts
    foreach (QInstaller::Component *component, components)
        component->loadComponentScript();

    // now set the checked state for all components without child
    for (int i = 0; i < rootComponentCount(AllMode); ++i) {
        QList<Component*> children = rootComponent(i, AllMode)->childs();
        foreach (Component *child, children) {
            if (child->isCheckable() && !child->isTristate()) {
                if (child->isInstalled() || child->isDefault())
                    child->setCheckState(Qt::Checked);
            }
        }
    }

    emit finishAllComponentsReset();

    return true;
}

bool PackageManagerCore::fetchUpdaterPackages()
{
    if (!isUpdater())
        return false;

    QHash<QString, KDUpdater::PackageInfo> installedPackages = localInstalledPackages();

    QScopedPointer <GetRepositoriesMetaInfoJob> metaInfoJob(fetchMetaInformation(d->m_installerSettings));
    if (metaInfoJob->isCanceled() || metaInfoJob->error() != KDJob::NoError) {
        if (metaInfoJob->error() != QInstaller::UserIgnoreError) {
            verbose() << tr("Could not retrieve updates: %1").arg(metaInfoJob->errorString()) << std::endl;
            return false;
        }
    }

    if (!metaInfoJob->temporaryDirectories().isEmpty()) {
        if (!addUpdateResourcesFrom(metaInfoJob.data(), d->m_installerSettings, true)) {
            verbose() << tr("Could not add temporary update source information.") << std::endl;
            return false;
        }
    }

    if (d->m_app->updateSourcesInfo()->updateSourceInfoCount() == 0) {
        verbose() << tr("Could not find any update source information.") << std::endl;
        return false;
    }

    KDUpdater::UpdateFinder updateFinder(d->m_app);
    updateFinder.setAutoDelete(false);
    updateFinder.setUpdateType(KDUpdater::PackageUpdate | KDUpdater::NewPackage);
    updateFinder.run();

    const QList<KDUpdater::Update*> &updates = updateFinder.updates();
    if (updates.isEmpty()) {
        verbose() << tr("Could not retrieve updates: %1").arg(updateFinder.errorString());
        return false;
    }

    emit startUpdaterComponentsReset();

    d->clearUpdaterComponentLists();
    QMap<QString, QInstaller::Component*> components;

    Data data;
    data.components = &components;
    data.metaInfoJob = metaInfoJob.data();
    data.installedPackages = &installedPackages;

    bool importantUpdates = false;
    foreach (KDUpdater::Update *update, updates) {
        QScopedPointer<QInstaller::Component> component(new QInstaller::Component(this));

        data.package = update;
        component->loadDataFromUpdate(update);
        if (updateComponentData(data, component.data())) {
            // Keep a reference so we can resolve dependencies during update.
            d->m_updaterComponentsDeps.append(component.take());

            const QString isNew = update->data(scNewComponent).toString();
            if (isNew.toLower() != scTrue)
                continue;

            const QString &name = d->m_updaterComponentsDeps.last()->name();
            const QString replaces = data.package->data(scReplaces).toString();
            bool isValidUpdate = installedPackages.contains(name);
            if (!isValidUpdate && !replaces.isEmpty()) {
                const QStringList possibleNames = replaces.split(QLatin1String(","), QString::SkipEmptyParts);
                foreach (const QString &possibleName, possibleNames)
                    isValidUpdate |= installedPackages.contains(possibleName);
            }

            if (!isValidUpdate)
                continue;   // Update for not installed package found, skip it.


            const KDUpdater::PackageInfo &info = installedPackages.value(name);
            const QString updateVersion = update->data(scVersion).toString();
            if (KDUpdater::compareVersion(updateVersion, info.version) <= 0)
                continue;

            // It is quite possible that we may have already installed the update. Lets check the last
            // update date of the package and the release date of the update. This way we can compare and
            // figure out if the update has been installed or not.
            const QDate updateDate = update->data(scReleaseDate).toDate();
            if (info.lastUpdateDate > updateDate)
                continue;

            // this is not a dependency, it is a real update
            components.insert(name, d->m_updaterComponentsDeps.takeLast());
        }
    }

    // store all components that got a replacement
    storeReplacedComponents(components, data.replacementToExchangeables);

    // remove all unimportant components
    QList<QInstaller::Component*> updaterComponents = components.values();
    if (importantUpdates) {
        for (int i = updaterComponents.count() - 1; i >= 0; --i) {
            if (updaterComponents.at(i)->value(scImportant, scFalse).toLower() == scFalse)
                delete updaterComponents.takeAt(i);
        }
    }

    if (!updaterComponents.isEmpty()) {
        // load the scripts and append all components w/o parent to the direct list
        foreach (QInstaller::Component *component, updaterComponents) {
            component->loadComponentScript();
            component->setCheckState(Qt::Checked);
            appendRootComponent(component, UpdaterMode);
        }

        // after everything is set up, check installed components
        foreach (QInstaller::Component *component, d->m_updaterComponentsDeps) {
            if (component->isInstalled()) {
                // since we do not put them into the model, which would force a update of e.g. tri state
                // components, we have to check all installed components ourself
                component->setCheckState(Qt::Checked);
            }
        }
    } else {
        // we have no updates, no need to store possible dependencies
        qDeleteAll(d->m_updaterComponentsDeps);
        d->m_updaterComponentsDeps.clear();
    }

    emit finishUpdaterComponentsReset();

    return true;
}

/*!
    Adds the widget with objectName() \a name registered by \a component as a new page
    into the installer's GUI wizard. The widget is added before \a page.
    \a page has to be a value of \ref QInstaller::PackageManagerCore::WizardPage "WizardPage".
*/
bool PackageManagerCore::addWizardPage(Component* component, const QString &name, int page)
{
    if (QWidget* const widget = component->userInterface(name)) {
        emit wizardPageInsertionRequested(widget, static_cast<WizardPage>(page));
        return true;
    }
    return false;
}

/*!
    Removes the widget with objectName() \a name previously added to the installer's wizard
    by \a component.
*/
bool PackageManagerCore::removeWizardPage(Component *component, const QString &name)
{
    if (QWidget* const widget = component->userInterface(name)) {
        emit wizardPageRemovalRequested(widget);
        return true;
    }
    return false;
}

/*!
    Sets the visibility of the default page with id \a page to \a visible, i.e.
    removes or adds it from/to the wizard. This works only for pages which have been
    in the installer when it was started.
 */
bool PackageManagerCore::setDefaultPageVisible(int page, bool visible)
{
    emit wizardPageVisibilityChangeRequested(visible, page);
    return true;
}

/*!
    Adds the widget with objectName() \a name registered by \a component as an GUI element
    into the installer's GUI wizard. The widget is added on \a page.
    \a page has to be a value of \ref QInstaller::PackageManagerCore::WizardPage "WizardPage".
*/
bool PackageManagerCore::addWizardPageItem(Component *component, const QString &name, int page)
{
    if (QWidget* const widget = component->userInterface(name)) {
        emit wizardWidgetInsertionRequested(widget, static_cast<WizardPage>(page));
        return true;
    }
    return false;
}

/*!
    Removes the widget with objectName() \a name previously added to the installer's wizard
    by \a component.
*/
bool PackageManagerCore::removeWizardPageItem(Component *component, const QString &name)
{
    if (QWidget* const widget = component->userInterface(name)) {
        emit wizardWidgetRemovalRequested(widget);
        return true;
    }
    return false;
}

void PackageManagerCore::addRepositories(const QList<Repository> &repositories)
{
    d->m_installerSettings.addUserRepositories(repositories);
}

/*!
    Sets additional repository for this instance of the installer or updater
    Will be removed after invoking it again
*/
void PackageManagerCore::setTemporaryRepositories(const QList<Repository> &repositories, bool replace)
{
    d->m_installerSettings.setTemporaryRepositories(repositories, replace);
}

/*!
    checks if the downloader should try to download sha1 checksums for archives
*/
bool PackageManagerCore::testChecksum() const
{
    return d->m_testChecksum;
}

/*!
    Defines if the downloader should try to download sha1 checksums for archives
*/
void PackageManagerCore::setTestChecksum(bool test)
{
    d->m_testChecksum = test;
}

/*!
    Returns the number of components in the list depending on the run mode \a runMode.
*/
int PackageManagerCore::rootComponentCount(RunMode runMode) const
{
    if (runMode == UpdaterMode)
        return d->m_updaterComponents.size();
    return d->m_rootComponents.size();
}

/*!
    Returns the component at index position i in the components list. i must be a valid index
    position in the list (i.e., 0 <= i < rootComponentCount(...)).
*/
Component *PackageManagerCore::rootComponent(int i, RunMode runMode) const
{
    if (runMode == UpdaterMode)
        return d->m_updaterComponents.value(i, 0);
    return d->m_rootComponents.value(i, 0);
}

/*!
    Appends a new root components \a component based on the current run mode \a runMode to the
    installers internal lists of components.
*/
void PackageManagerCore::appendRootComponent(Component *component, RunMode runMode)
{
    if (runMode == AllMode)
        d->m_rootComponents.append(component);
    else
        d->m_updaterComponents.append(component);
    emit componentAdded(component);
}

/*!
    Returns a component matching \a name. \a name can also contains a version requirement.
    E.g. "com.nokia.sdk.qt" returns any component with that name, "com.nokia.sdk.qt->=4.5" requires
    the returned component to have at least version 4.5.
    If no component matches the requirement, 0 is returned.
*/
Component* PackageManagerCore::componentByName(const QString &name) const
{
    if (name.isEmpty())
        return 0;

    if (name.contains(QChar::fromLatin1('-'))) {
        // the last part is considered to be the version, then
        const QString version = name.section(QLatin1Char('-'), 1);
        return subComponentByName(this, name.section(QLatin1Char('-'), 0, 0), version);
    }

    return subComponentByName(this, name);
}

QList<Component*> PackageManagerCore::components(bool recursive, RunMode runMode) const
{
    if (runMode == UpdaterMode)
        return d->m_updaterComponents;

    if (!recursive)
        return d->m_rootComponents;

    QList<Component*> result;
    foreach (QInstaller::Component *component, d->m_rootComponents) {
        result.push_back(component);
        result += component->childComponents(true, runMode);
    }

    return result;
}

QList<Component*> PackageManagerCore::componentsToInstall(RunMode runMode) const
{
    QList<Component*> availableComponents = components(true, runMode);
    std::sort(availableComponents.begin(), availableComponents.end(),
        Component::InstallPriorityLessThan());

    QList<Component*> componentsToInstall;
    foreach (QInstaller::Component *component, availableComponents) {
        if (!component->installationRequested())
            continue;
        appendComponentAndMissingDependencies(componentsToInstall, component);
    }

    return componentsToInstall;
}

/*!
    Returns a list of packages depending on \a component.
*/
QList<Component*> PackageManagerCore::dependees(const Component *component) const
{
    QList<Component*> allComponents = components(true, runMode());
    if (runMode() == UpdaterMode)
        allComponents += d->m_updaterComponentsDeps;

    QList<Component*> result;
    foreach (Component *comp, allComponents) {
        const QStringList dependencies = comp->value(scDependencies).split(QChar::fromLatin1(','),
            QString::SkipEmptyParts);

        const QLatin1Char dash('-');
        foreach (const QString &dependency, dependencies) {
            // the last part is considered to be the version, then
            const QString name = dependency.contains(dash) ? dependency.section(dash, 0, 0) : dependency;
            const QString version = dependency.contains(dash) ? dependency.section(dash, 1) : QString();
            if (componentMatches(component, name, version))
                result.append(comp);
        }
    }

    return result;
}

/*!
    Returns the list of all missing (not installed) dependencies for \a component.
*/
QList<Component*> PackageManagerCore::missingDependencies(const Component *component) const
{
    QList<Component*> allComponents = components(true, runMode());
    if (runMode() == UpdaterMode)
        allComponents += d->m_updaterComponentsDeps;

    const QStringList dependencies = component->value(scDependencies).split(QChar::fromLatin1(','),
        QString::SkipEmptyParts);

    QList<Component*> result;
    const QLatin1Char dash('-');
    foreach (const QString &dependency, dependencies) {
        const bool hasVersionString = dependency.contains(dash);
        const QString name = hasVersionString ? dependency.section(dash, 0, 0) : dependency;

        bool installed = false;
        foreach (Component *comp, allComponents) {
            if (comp->name() == name) {
                if (hasVersionString) {
                    const QString version = dependency.section(dash, 1);
                    if (PackageManagerCore::versionMatches(comp->value(scInstalledVersion), version))
                        installed = true;
                } else if (comp->isInstalled()) {
                    installed = true;
                }
            }
        }

        if (!installed) {
            if (Component *comp = componentByName(dependency))
                result.append(comp);
        }
    }
    return result;
}

/*!
    Returns a list of dependencies for \a component. If there's a dependency which cannot be fulfilled,
    \a missingComponents will contain the missing components.
*/
QList<Component*> PackageManagerCore::dependencies(const Component *component, QStringList &missingComponents) const
{
    QList<Component*> result;
    const QStringList dependencies = component->value(scDependencies).split(QChar::fromLatin1(','),
        QString::SkipEmptyParts);

    foreach (const QString &dependency, dependencies) {
        Component *component = componentByName(dependency);
        if (component)
            result.append(component);
        else
            missingComponents.append(dependency);
    }
    return result;
}

const InstallerSettings &PackageManagerCore::settings() const
{
    return d->m_installerSettings;
}

/*!
    This method tries to gain admin rights. On success, it returns true.
*/
bool PackageManagerCore::gainAdminRights()
{
    if (AdminAuthorization::hasAdminRights())
        return true;

    d->m_FSEngineClientHandler->setActive(true);
    if (!d->m_FSEngineClientHandler->isActive())
        throw Error(QObject::tr("Error while elevating access rights."));
    return true;
}

/*!
    This method drops gained admin rights.
*/
void PackageManagerCore::dropAdminRights()
{
    d->m_FSEngineClientHandler->setActive(false);
}

/*!
    Return true, if a process with \a name is running. On Windows, the comparison
    is case-insensitive.
*/
bool PackageManagerCore::isProcessRunning(const QString &name) const
{
    return PackageManagerCorePrivate::isProcessRunning(name, KDSysInfo::runningProcesses());
}

/*!
    Executes a program.

    \param program The program that should be executed.
    \param arguments Optional list of arguments.
    \param stdIn Optional stdin the program reads.
    \return If the command could not be executed, an empty QList, otherwise the output of the
            command as first item, the return code as second item.
    \note On Unix, the output is just the output to stdout, not to stderr.
*/

QList<QVariant> PackageManagerCore::execute(const QString &program, const QStringList &arguments,
    const QString &stdIn) const
{
    QEventLoop loop;
    QProcessWrapper p;

    connect(&p, SIGNAL(finished(int, QProcess::ExitStatus)), &loop, SLOT(quit()));
    p.start(program, arguments, stdIn.isNull() ? QIODevice::ReadOnly : QIODevice::ReadWrite);

    if (!p.waitForStarted())
        return QList< QVariant >();

    if (!stdIn.isNull()) {
        p.write(stdIn.toLatin1());
        p.closeWriteChannel();
    }

    if (p.state() != QProcessWrapper::NotRunning)
        loop.exec();

    return QList< QVariant >() << QString::fromLatin1(p.readAllStandardOutput()) << p.exitCode();
}

/*!
    Returns an environment variable.
*/
QString PackageManagerCore::environmentVariable(const QString &name) const
{
#ifdef Q_WS_WIN
    const LPCWSTR n =  (LPCWSTR) name.utf16();
    LPTSTR buff = (LPTSTR) malloc(4096 * sizeof(TCHAR));
    DWORD getenvret = GetEnvironmentVariable(n, buff, 4096);
    const QString actualValue = getenvret != 0
        ? QString::fromUtf16((const unsigned short *) buff) : QString();
    free(buff);
    return actualValue;
#else
    const char *pPath = name.isEmpty() ? 0 : getenv(name.toLatin1());
    return pPath ? QLatin1String(pPath) : QString();
#endif
}

/*!
    Instantly performs an operation \a name with \a arguments.
    \sa Component::addOperation
*/
bool PackageManagerCore::performOperation(const QString &name, const QStringList &arguments)
{
    QScopedPointer<KDUpdater::UpdateOperation> op(KDUpdater::UpdateOperationFactory::instance()
        .create(name));
    if (!op.data())
        return false;

    op->setArguments(arguments);
    op->backup();
    if (!PackageManagerCorePrivate::performOperationThreaded(op.data())) {
        PackageManagerCorePrivate::performOperationThreaded(op.data(), PackageManagerCorePrivate::Undo);
        return false;
    }
    return true;
}

/*!
    Returns true when \a version matches the \a requirement.
    \a requirement can be a fixed version number or it can be prefix by the comparators '>', '>=',
    '<', '<=' and '='.
*/
bool PackageManagerCore::versionMatches(const QString &version, const QString &requirement)
{
    QRegExp compEx(QLatin1String("([<=>]+)(.*)"));
    const QString comparator = compEx.exactMatch(requirement) ? compEx.cap(1) : QString::fromLatin1("=");
    const QString ver = compEx.exactMatch(requirement) ? compEx.cap(2) : requirement;

    const bool allowEqual = comparator.contains(QLatin1Char('='));
    const bool allowLess = comparator.contains(QLatin1Char('<'));
    const bool allowMore = comparator.contains(QLatin1Char('>'));

    if (allowEqual && version == ver)
        return true;

    if (allowLess && KDUpdater::compareVersion(ver, version) > 0)
        return true;

    if (allowMore && KDUpdater::compareVersion(ver, version) < 0)
        return true;

    return false;
}

/*!
    Finds a library named \a name in \a pathes.
    If \a pathes is empty, it gets filled with platform dependent default pathes.
    The resulting path is stored in \a library.
    This method can be used by scripts to check external dependencies.
*/
QString PackageManagerCore::findLibrary(const QString &name, const QStringList &pathes)
{
    QStringList findPathes = pathes;
#if defined(Q_WS_WIN)
    return findPath(QString::fromLatin1("%1.lib").arg(name), findPathes);
#else
    if (findPathes.isEmpty()) {
        findPathes.push_back(QLatin1String("/lib"));
        findPathes.push_back(QLatin1String("/usr/lib"));
        findPathes.push_back(QLatin1String("/usr/local/lib"));
        findPathes.push_back(QLatin1String("/opt/local/lib"));
    }
#if defined(Q_WS_MAC)
    const QString dynamic = findPath(QString::fromLatin1("lib%1.dylib").arg(name), findPathes);
#else
    const QString dynamic = findPath(QString::fromLatin1("lib%1.so*").arg(name), findPathes);
#endif
    if (!dynamic.isEmpty())
        return dynamic;
    return findPath(QString::fromLatin1("lib%1.a").arg(name), findPathes);
#endif
}

/*!
    Tries to find a file name \a name in one of \a pathes.
    The resulting path is stored in \a path.
    This method can be used by scripts to check external dependencies.
*/
QString PackageManagerCore::findPath(const QString &name, const QStringList &pathes)
{
    foreach (const QString &path, pathes) {
        const QDir dir(path);
        const QStringList entries = dir.entryList(QStringList() << name, QDir::Files | QDir::Hidden);
        if (entries.isEmpty())
            continue;

        return dir.absoluteFilePath(entries.first());
    }
    return QString();
}

/*!
    sets the "installerbase" binary to use when writing the package manager/uninstaller.
    Set this if an update to installerbase is available.
    If not set, the executable segment of the running un/installer will be used.
*/
void PackageManagerCore::setInstallerBaseBinary(const QString &path)
{
    d->m_forceRestart = true;
    d->m_installerBaseBinaryUnreplaced = path;
}

/*!
    Returns the installer value for \a key. If \a key is not known to the system, \a defaultValue is
    returned. Additionally, on Windows, \a key can be a registry key.
*/
QString PackageManagerCore::value(const QString &key, const QString &defaultValue) const
{
#ifdef Q_WS_WIN
    if (!d->m_vars.contains(key)) {
        static const QRegExp regex(QLatin1String("\\\\|/"));
        const QString filename = key.section(regex, 0, -2);
        const QString regKey = key.section(regex, -1);
        const QSettingsWrapper registry(filename, QSettingsWrapper::NativeFormat);
        if (!filename.isEmpty() && !regKey.isEmpty() && registry.contains(regKey))
            return registry.value(regKey).toString();
    }
#else
    if (key == scTargetDir) {
        const QString dir = d->m_vars.value(key, defaultValue);
        if (dir.startsWith(QLatin1String("~/")))
            return QDir::home().absoluteFilePath(dir.mid(2));
        return dir;
    }
#endif
    return d->m_vars.value(key, defaultValue);
}

/*!
    Sets the installer value for \a key to \a value.
*/
void PackageManagerCore::setValue(const QString &key, const QString &value)
{
    if (d->m_vars.value(key) == value)
        return;

    d->m_vars.insert(key, value);
    emit valueChanged(key, value);
}

/*!
    Returns true, when the installer contains a value for \a key.
*/
bool PackageManagerCore::containsValue(const QString &key) const
{
    return d->m_vars.contains(key);
}

void PackageManagerCore::setSharedFlag(const QString &key, bool value)
{
    d->m_sharedFlags.insert(key, value);
}

bool PackageManagerCore::sharedFlag(const QString &key) const
{
    return d->m_sharedFlags.value(key, false);
}

bool PackageManagerCore::isVerbose() const
{
    return QInstaller::isVerbose();
}

void PackageManagerCore::setVerbose(bool on)
{
    QInstaller::setVerbose(on);
}

PackageManagerCore::Status PackageManagerCore::status() const
{
    return PackageManagerCore::Status(d->m_status);
}
/*!
    Returns true if at least one complete installation/update was successful, even if the user cancelled the
    newest installation process.
*/
bool PackageManagerCore::finishedWithSuccess() const
{
    return (d->m_status == PackageManagerCore::Success) || d->m_needToWriteUninstaller;
}

void PackageManagerCore::interrupt()
{
    setCanceled();
    emit installationInterrupted();
}

void PackageManagerCore::setCanceled()
{
    d->setStatus(PackageManagerCore::Canceled);
    emit cancelMetaInfoJob();
}

/*!
    Replaces all variables within \a str by their respective values and returns the result.
*/
QString PackageManagerCore::replaceVariables(const QString &str) const
{
    return d->replaceVariables(str);
}

/*!
    \overload
    Replaces all variables in any of \a str by their respective values and returns the results.
*/
QStringList PackageManagerCore::replaceVariables(const QStringList &str) const
{
    QStringList result;
    foreach (const QString &s, str)
        result.push_back(d->replaceVariables(s));

    return result;
}

/*!
    \overload
    Replaces all variables within \a ba by their respective values and returns the result.
*/
QByteArray PackageManagerCore::replaceVariables(const QByteArray &ba) const
{
    return d->replaceVariables(ba);
}

/*!
    Returns the path to the installer binary.
*/
QString PackageManagerCore::installerBinaryPath() const
{
    return d->installerBinaryPath();
}

/*!
    Returns true when this is the installer running.
*/
bool PackageManagerCore::isInstaller() const
{
    return d->isInstaller();
}

/*!
    Returns true if this is an offline-only installer.
*/
bool PackageManagerCore::isOfflineOnly() const
{
    QSettingsWrapper confInternal(QLatin1String(":/config/config-internal.ini"), QSettingsWrapper::IniFormat);
    return confInternal.value(QLatin1String("offlineOnly")).toBool();
}

void PackageManagerCore::setUninstaller()
{
    d->m_magicBinaryMarker = QInstaller::MagicUninstallerMarker;
}

/*!
    Returns true when this is the uninstaller running.
*/
bool PackageManagerCore::isUninstaller() const
{
    return d->isUninstaller();
}

void PackageManagerCore::setUpdater()
{
    d->m_magicBinaryMarker = QInstaller::MagicUpdaterMarker;
}

/*!
    Returns true when this is neither an installer nor an uninstaller running.
    Must be an updater, then.
*/
bool PackageManagerCore::isUpdater() const
{
    return d->isUpdater();
}

void PackageManagerCore::setPackageManager()
{
    d->m_magicBinaryMarker = QInstaller::MagicPackageManagerMarker;
}

/*!
    Returns true when this is the package manager running.
*/
bool PackageManagerCore::isPackageManager() const
{
    return d->isPackageManager();
}

/*!
    Runs the installer. Returns true on success, false otherwise.
*/
bool PackageManagerCore::runInstaller()
{
    try {
        d->runInstaller();
        return true;
    } catch (...) {
        return false;
    }
}

/*!
    Runs the uninstaller. Returns true on success, false otherwise.
*/
bool PackageManagerCore::runUninstaller()
{
    try {
        d->runUninstaller();
        return true;
    } catch (...) {
        return false;
    }
}

/*!
    Runs the package updater. Returns true on success, false otherwise.
*/
bool PackageManagerCore::runPackageUpdater()
{
    try {
        d->runPackageUpdater();
        return true;
    } catch (...) {
        return false;
    }
}

/*!
    \internal
    Calls languangeChanged on all components.
*/
void PackageManagerCore::languageChanged()
{
    const QList<Component*> comps = components(true, runMode());
    foreach (Component* component, comps)
        component->languageChanged();
}

/*!
    Runs the installer, un-installer, updater or package manager, depending on the type of this binary.
*/
bool PackageManagerCore::run()
{
    try {
        if (isInstaller())
            d->runInstaller();
        else if (isUninstaller())
            d->runUninstaller();
        else if (isPackageManager() || isUpdater())
            d->runPackageUpdater();
        return true;
    } catch (const Error &err) {
        verbose() << "Caught Installer Error: " << err.message() << std::endl;
        return false;
    }
}

/*!
    Returns the path name of the uninstaller binary.
*/
QString PackageManagerCore::uninstallerName() const
{
    return d->uninstallerName();
}

bool PackageManagerCore::setAndParseLocalComponentsFile(KDUpdater::PackagesInfo &packagesInfo)
{
    packagesInfo.setFileName(d->localComponentsXmlPath());
    const QString localComponentsXml = d->localComponentsXmlPath();

    // handle errors occurred by loading components.xml
    QFileInfo componentFileInfo(localComponentsXml);
    int silentRetries = d->m_silentRetries;
    while (!componentFileInfo.exists()) {
        if (silentRetries > 0) {
            --silentRetries;
        } else {
            Status status = handleComponentsFileSetOrParseError(localComponentsXml);
            if (status == PackageManagerCore::Canceled)
                return false;
        }
        packagesInfo.setFileName(localComponentsXml);
    }

    silentRetries = d->m_silentRetries;
    while (packagesInfo.error() != KDUpdater::PackagesInfo::NoError) {
        if (silentRetries > 0) {
            --silentRetries;
        } else {
            Status status = handleComponentsFileSetOrParseError(localComponentsXml);
            if (status == PackageManagerCore::Canceled)
                return false;
        }
        packagesInfo.setFileName(localComponentsXml);
    }

    silentRetries = d->m_silentRetries;
    while (packagesInfo.error() != KDUpdater::PackagesInfo::NoError) {
        if (silentRetries > 0) {
            --silentRetries;
        } else {
            bool retry = false;
            if (packagesInfo.error() != KDUpdater::PackagesInfo::InvalidContentError
                && packagesInfo.error() != KDUpdater::PackagesInfo::InvalidXmlError) {
                    retry = true;
            }
            Status status = handleComponentsFileSetOrParseError(componentFileInfo.fileName(),
                packagesInfo.errorString(), retry);
            if (status == PackageManagerCore::Canceled)
                return false;
        }
        packagesInfo.setFileName(localComponentsXml);
    }

    return true;
}

PackageManagerCore::Status PackageManagerCore::handleComponentsFileSetOrParseError(const QString &arg1,
    const QString &arg2, bool withRetry)
{
    QMessageBox::StandardButtons buttons = QMessageBox::Cancel;
    if (withRetry)
        buttons |= QMessageBox::Retry;

    const QMessageBox::StandardButton button =
        MessageBoxHandler::critical(MessageBoxHandler::currentBestSuitParent(),
        QLatin1String("Error loading component.xml"), tr("Loading error"),
        tr(arg2.isEmpty() ? "Could not load %1" : "Could not load %1 : %2").arg(arg1, arg2),
        buttons);

    if (button == QMessageBox::Cancel) {
        d->m_status = PackageManagerCore::Failure;
        return PackageManagerCore::Canceled;
    }
    return PackageManagerCore::Unfinished;
}

bool PackageManagerCore::updateComponentData(struct Data &data, Component *component)
{
    try {
        const QString name = data.package->data(scName).toString();
        if (data.components->contains(name)) {
            qCritical("Could not register component! Component with identifier %s already registered.",
                qPrintable(name));
            return false;
        }

        if (!data.installedPackages->contains(name)) {
            const QString replaces = data.package->data(scReplaces).toString();
            if (!replaces.isEmpty()) {
                const QStringList components = replaces.split(QLatin1Char(','), QString::SkipEmptyParts);
                foreach (const QString &componentName, components) {
                    if (data.installedPackages->contains(componentName)) {
                        if (runMode() == AllMode) {
                            component->setInstalled();
                            component->setValue(scInstalledVersion, data.package->data(scVersion).toString());
                        }
                        data.replacementToExchangeables.insert(component, components);
                        break;  // break as soon as we know we replace at least one other component
                    } else {
                        component->setUninstalled();
                    }
                }
            } else {
                component->setUninstalled();
            }
        } else {
            component->setInstalled();
            component->setValue(scInstalledVersion, data.installedPackages->value(name).version);
        }

        const QString &localPath = component->localTempPath();
        if (isVerbose()) {
            static QString lastLocalPath;
            if (lastLocalPath != localPath)
                verbose() << "Url is : " << localPath << std::endl;
            lastLocalPath = localPath;
        }
        component->setRepositoryUrl(data.metaInfoJob->repositoryForTemporaryDirectory(localPath).url());
    } catch (...) {
        return false;
    }

    return true;
}

void PackageManagerCore::storeReplacedComponents(QMap<QString, Component*> &components,
    const QHash<Component*, QStringList> &replacementToExchangeables)
{
    QHash<Component*, QStringList>::const_iterator it;
    // remeber all components that got a replacement, requierd for uninstall
    for (it = replacementToExchangeables.constBegin(); it != replacementToExchangeables.constEnd(); ++it) {
        foreach (const QString &componentName, it.value()) {
            Component *component = components.take(componentName);
            if (!component && !d->componentsToReplace().contains(componentName)) {
                component = new Component(this);
                component->setValue(scName, componentName);
            }
            if (component)
                d->componentsToReplace().insert(componentName, qMakePair(it.key(), component));
        }
    }
}