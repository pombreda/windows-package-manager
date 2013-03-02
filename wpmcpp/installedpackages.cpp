#include "installedpackages.h"

#include <windows.h>
#include <QDebug>
#include <msi.h>

#include "windowsregistry.h"
#include "package.h"
#include "version.h"
#include "packageversion.h"
#include "repository.h"
#include "wpmutils.h"

InstalledPackages InstalledPackages::def;

InstalledPackages* InstalledPackages::getDefault()
{
    return &def;
}

InstalledPackages::InstalledPackages()
{
}

InstalledPackageVersion* InstalledPackages::find(const QString& package,
        const Version& version) const
{
    return this->data.value(PackageVersion::getStringId(package, version));
}

InstalledPackageVersion* InstalledPackages::findOrCreate(const QString& package,
        const Version& version)
{
    QString key = PackageVersion::getStringId(package, version);
    InstalledPackageVersion* r = this->data.value(key);
    if (!r) {
        r = new InstalledPackageVersion(package, version, "");
        this->data.insert(key, r);

        // qDebug() << "InstalledPackages::findOrCreate " << package;
        r->save();
    }
    return r;
}

QString InstalledPackages::setPackageVersionPath(const QString& package,
        const Version& version,
        const QString& directory)
{
    QString err;

    InstalledPackageVersion* ipv = this->find(package, version);
    if (!ipv) {
        ipv = new InstalledPackageVersion(package, version, directory);
        this->data.insert(package + "/" + version.getVersionString(), ipv);
        err = ipv->save();
    } else {
        ipv->setPath(directory);
    }

    /*Repository::getDefault()->fireStatusChanged(
            PackageVersion::getStringId(package, version)); TODO: */

    return err;
}

void InstalledPackages::setPackageVersionPathIfNotInstalled(
        const QString& package,
        const Version& version,
        const QString& directory)
{
    InstalledPackageVersion* ipv = findOrCreate(package, version);
    if (!ipv->installed())
        ipv->setPath(directory);
}

QList<InstalledPackageVersion*> InstalledPackages::getAll() const
{
    return this->data.values();
}

QStringList InstalledPackages::getAllInstalledPackagePaths() const
{
    QStringList r;
    QList<InstalledPackageVersion*> ipvs = this->data.values();
    for (int i = 0; i < ipvs.count(); i++) {
        InstalledPackageVersion* ipv = ipvs.at(i);
        if (ipv->installed())
            r.append(ipv->getDirectory());
    }
    return r;
}

void InstalledPackages::refresh(Job *job)
{

    if (!job->isCancelled() && job->getErrorMessage().isEmpty()) {
        job->setHint("Detecting directories deleted externally");
        QList<InstalledPackageVersion*> ipvs = this->data.values();
        for (int i = 0; i < ipvs.count(); i++) {
            InstalledPackageVersion* ipv = ipvs.at(i);
            if (ipv->installed()) {
                QDir d(ipv->getDirectory());
                d.refresh();
                if (!d.exists()) {
                    ipv->setPath("");
                }
            }
        }
        job->setProgress(0.2);
    }

    if (!job->isCancelled() && job->getErrorMessage().isEmpty()) {
        job->setHint("Detecting packages installed by Npackd 1.14 or earlier");
        detectPre_1_15_Packages();
        job->setProgress(0.4);
    }

    if (!job->isCancelled() && job->getErrorMessage().isEmpty()) {
        job->setHint("Reading registry package database");
        readRegistryDatabase();
        job->setProgress(0.5);
    }

    if (!job->isCancelled() && job->getErrorMessage().isEmpty()) {
        job->setHint("Detecting software");
        Job* d = job->newSubJob(0.2);
        detect(d);
        delete d;
    }

    if (!job->isCancelled() && job->getErrorMessage().isEmpty()) {
        job->setHint("Detecting packages installed by Npackd 1.14 or earlier (2)");
        scanPre1_15Dir(true);
        job->setProgress(0.9);
    }

    if (job->shouldProceed(
            "Clearing information about installed package versions in nested directories")) {
        clearPackagesInNestedDirectories();
        job->setProgress(1);
    }

    job->complete();
}

void InstalledPackages::fireStatusChanged(const QString &package,
        const Version &version)
{
    emit statusChanged(package, version);
}

void InstalledPackages::clearPackagesInNestedDirectories() {
    /* TODO:
    QList<PackageVersion*> pvs = this->getInstalled();
    qSort(pvs.begin(), pvs.end(), packageVersionLessThan2);

    for (int j = 0; j < pvs.count(); j++) {
        PackageVersion* pv = pvs.at(j);
        if (pv->installed() && !WPMUtils::pathEquals(pv->getPath(),
                WPMUtils::getWindowsDir())) {
            for (int i = j + 1; i < pvs.count(); i++) {
                PackageVersion* pv2 = pvs.at(i);
                if (pv2->installed() && !WPMUtils::pathEquals(pv2->getPath(),
                        WPMUtils::getWindowsDir())) {
                    if (WPMUtils::isUnder(pv2->getPath(), pv->getPath()) ||
                            WPMUtils::pathEquals(pv2->getPath(), pv->getPath())) {
                        pv2->setPath("");
                    }
                }
            }
        }
    }
    */
}

void InstalledPackages::readRegistryDatabase()
{
    this->data.clear();

    WindowsRegistry machineWR(HKEY_LOCAL_MACHINE, false, KEY_READ);

    AbstractRepository* rep = AbstractRepository::getDefault_();

    QString err;
    WindowsRegistry packagesWR;
    err = packagesWR.open(machineWR,
            "SOFTWARE\\Npackd\\Npackd\\Packages", KEY_READ);
    if (err.isEmpty()) {
        QStringList entries = packagesWR.list(&err);
        for (int i = 0; i < entries.count(); ++i) {
            QString name = entries.at(i);
            int pos = name.lastIndexOf("-");
            if (pos > 0) {
                QString packageName = name.left(pos);
                if (Package::isValidName(packageName)) {
                    QString versionName = name.right(name.length() - pos - 1);
                    Version version;
                    if (version.setVersion(versionName)) {
                        rep->addPackageVersion(packageName, version);
                        InstalledPackageVersion* ipv = this->find(
                                packageName, version);
                        if (!ipv) {
                            ipv = new InstalledPackageVersion(
                                    packageName, version, "");
                            this->data.insert(PackageVersion::getStringId(
                                    packageName, version), ipv);
                        }

                        // qDebug() << "loading " << packageName << ":" <<
                        //version.getVersionString();

                        ipv->loadFromRegistry();
                    }
                }
            }
        }
    }
}

void InstalledPackages::detect(Job* job)
{
    job->setProgress(0);

    if (!job->isCancelled()) {
        job->setHint("Detecting Windows");
        detectWindows();
        job->setProgress(0.01);
    }

    if (!job->isCancelled()) {
        job->setHint("Detecting JRE");
        detectJRE(false);
        if (WPMUtils::is64BitWindows())
            detectJRE(true);
        job->setProgress(0.1);
    }

    if (!job->isCancelled()) {
        job->setHint("Detecting JDK");
        detectJDK(false);
        if (WPMUtils::is64BitWindows())
            detectJDK(true);
        job->setProgress(0.2);
    }

    if (!job->isCancelled()) {
        job->setHint("Detecting .NET");
        detectDotNet();
        job->setProgress(0.3);
    }

    // MSI package detection should happen before the detection for
    // control panel programs
    if (!job->isCancelled()) {
        job->setHint("Detecting MSI packages");
        detectMSIProducts();
        job->setProgress(0.5);
    }

    if (!job->isCancelled()) {
        job->setHint("Detecting Control Panel programs");
        detectControlPanelPrograms();
        job->setProgress(0.9);
    }

    if (!job->isCancelled()) {
        job->setHint("Detecting Windows Installer");
        detectMicrosoftInstaller();
        job->setProgress(0.95);
    }

    if (!job->isCancelled()) {
        job->setHint("Detecting Microsoft Core XML Services (MSXML)");
        detectMSXML();
        job->setProgress(0.97);
    }

    if (!job->isCancelled()) {
        job->setHint("Updating NPACKD_CL");
        AbstractRepository* rep = AbstractRepository::getDefault_();
        rep->updateNpackdCLEnvVar();
        job->setProgress(1);
    }

    job->complete();
}

void InstalledPackages::detectJRE(bool w64bit)
{
    if (w64bit && !WPMUtils::is64BitWindows())
        return;

    AbstractRepository* rep = AbstractRepository::getDefault_();
    WindowsRegistry jreWR;
    QString err = jreWR.open(HKEY_LOCAL_MACHINE,
            "Software\\JavaSoft\\Java Runtime Environment", !w64bit, KEY_READ);
    if (err.isEmpty()) {
        QStringList entries = jreWR.list(&err);
        for (int i = 0; i < entries.count(); i++) {
            QString v_ = entries.at(i);
            v_ = v_.replace('_', '.');
            Version v;
            if (!v.setVersion(v_) || v.getNParts() <= 2)
                continue;

            WindowsRegistry wr;
            err = wr.open(jreWR, entries.at(i), KEY_READ);
            if (!err.isEmpty())
                continue;

            QString path = wr.get("JavaHome", &err);
            if (!err.isEmpty())
                continue;

            QDir d(path);
            if (!d.exists())
                continue;

            QString package = w64bit ? "com.oracle.JRE64" :
                    "com.oracle.JRE";
            rep->addPackageVersion(package, v);
            InstalledPackageVersion* ipv = InstalledPackages::getDefault()->
                    findOrCreate(package, v);
            if (!ipv->installed()) {
                ipv->setPath(path);
            }
        }
    }
}

void InstalledPackages::detectJDK(bool w64bit)
{
    QString p = w64bit ? "com.oracle.JDK64" : "com.oracle.JDK";

    if (w64bit && !WPMUtils::is64BitWindows())
        return;

    AbstractRepository* rep = AbstractRepository::getDefault_();
    WindowsRegistry wr;
    QString err = wr.open(HKEY_LOCAL_MACHINE,
            "Software\\JavaSoft\\Java Development Kit",
            !w64bit, KEY_READ);
    if (err.isEmpty()) {
        QStringList entries = wr.list(&err);
        if (err.isEmpty()) {
            for (int i = 0; i < entries.count(); i++) {
                QString v_ = entries.at(i);
                WindowsRegistry r;
                err = r.open(wr, v_, KEY_READ);
                if (!err.isEmpty())
                    continue;

                v_.replace('_', '.');
                Version v;
                if (!v.setVersion(v_) || v.getNParts() <= 2)
                    continue;

                QString path = r.get("JavaHome", &err);
                if (!err.isEmpty())
                    continue;

                QDir d(path);
                if (!d.exists())
                    continue;

                rep->addPackageVersion(p, v);
                InstalledPackageVersion* ipv = InstalledPackages::getDefault()->
                        findOrCreate(p, v);
                if (!ipv->installed()) {
                    ipv->setPath(path);
                }
            }
        }
    }
}

void InstalledPackages::detectWindows()
{
    OSVERSIONINFO osvi;
    osvi.dwOSVersionInfoSize = sizeof(OSVERSIONINFO);
    GetVersionEx(&osvi);
    Version v;
    v.setVersion(osvi.dwMajorVersion, osvi.dwMinorVersion,
            osvi.dwBuildNumber);

    AbstractRepository* rep = AbstractRepository::getDefault_();
    rep->addPackageVersion("com.microsoft.Windows", v);
    setPackageVersionPath("com.microsoft.Windows", v,
            WPMUtils::getWindowsDir());

    if (WPMUtils::is64BitWindows()) {
        rep->addPackageVersion("com.microsoft.Windows64", v);
        setPackageVersionPath("com.microsoft.Windows64", v,
                WPMUtils::getWindowsDir());
    } else {
        rep->addPackageVersion("com.microsoft.Windows32", v);
        setPackageVersionPath("com.microsoft.Windows32", v,
                WPMUtils::getWindowsDir());
    }
}

void InstalledPackages::detectOneDotNet(const WindowsRegistry& wr,
        const QString& keyName)
{
    QString packageName("com.microsoft.DotNetRedistributable");
    Version keyVersion;

    Version oneOne(1, 1);
    Version four(4, 0);
    Version two(2, 0);

    Version v;
    bool found = false;
    if (keyName.startsWith("v") && keyVersion.setVersion(
            keyName.right(keyName.length() - 1))) {
        if (keyVersion.compare(oneOne) < 0) {
            // not yet implemented
        } else if (keyVersion.compare(two) < 0) {
            v = keyVersion;
            found = true;
        } else if (keyVersion.compare(four) < 0) {
            QString err;
            QString value_ = wr.get("Version", &err);
            if (err.isEmpty() && v.setVersion(value_)) {
                found = true;
            }
        } else {
            WindowsRegistry r;
            QString err = r.open(wr, "Full", KEY_READ);
            if (err.isEmpty()) {
                QString value_ = r.get("Version", &err);
                if (err.isEmpty() && v.setVersion(value_)) {
                    found = true;
                }
            }
        }
    }

    if (found) {
        AbstractRepository* rep = AbstractRepository::getDefault_();
        rep->addPackageVersion(packageName, v);
        InstalledPackageVersion* ipv = findOrCreate(packageName, v);
        if (!ipv->installed()) {
            ipv->setPath(WPMUtils::getWindowsDir());
        }
    }
}

void InstalledPackages::detectControlPanelPrograms()
{
    QStringList packagePaths = getAllInstalledPackagePaths();

    QStringList foundDetectionInfos;

    detectControlPanelProgramsFrom(HKEY_LOCAL_MACHINE,
            "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
            false, &packagePaths, &foundDetectionInfos
    );
    if (WPMUtils::is64BitWindows())
        detectControlPanelProgramsFrom(HKEY_LOCAL_MACHINE,
                "SOFTWARE\\WoW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
                false, &packagePaths, &foundDetectionInfos
        );
    detectControlPanelProgramsFrom(HKEY_CURRENT_USER,
            "SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
            false, &packagePaths, &foundDetectionInfos
    );
    if (WPMUtils::is64BitWindows())
        detectControlPanelProgramsFrom(HKEY_CURRENT_USER,
                "SOFTWARE\\WoW6432Node\\Microsoft\\Windows\\CurrentVersion\\Uninstall",
                false, &packagePaths, &foundDetectionInfos
        );

    // remove uninstalled packages
    QMapIterator<QString, InstalledPackageVersion*> i(data);
    while (i.hasNext()) {
        i.next();
        InstalledPackageVersion* ipv = i.value();
        if (ipv->detectionInfo.indexOf("control-panel:") == 0 &&
                ipv->installed() &&
                !foundDetectionInfos.contains(ipv->detectionInfo)) {
            qDebug() << "control-panel package removed: " << ipv->package;
            ipv->setPath("");
        }
    }
}

void InstalledPackages::detectControlPanelProgramsFrom(HKEY root,
        const QString& path, bool useWoWNode, QStringList* packagePaths,
        QStringList* foundDetectionInfos) {
    WindowsRegistry wr;
    QString err;
    err = wr.open(root, path, useWoWNode, KEY_READ);
    if (err.isEmpty()) {
        QString fullPath;
        if (root == HKEY_CLASSES_ROOT)
            fullPath = "HKEY_CLASSES_ROOT";
        else if (root == HKEY_CURRENT_USER)
            fullPath = "HKEY_CURRENT_USER";
        else if (root == HKEY_LOCAL_MACHINE)
            fullPath = "HKEY_LOCAL_MACHINE";
        else if (root == HKEY_USERS)
            fullPath = "HKEY_USERS";
        else if (root == HKEY_PERFORMANCE_DATA)
            fullPath = "HKEY_PERFORMANCE_DATA";
        else if (root == HKEY_CURRENT_CONFIG)
            fullPath = "HKEY_CURRENT_CONFIG";
        else if (root == HKEY_DYN_DATA)
            fullPath = "HKEY_DYN_DATA";
        else
            fullPath = QString("%1").arg((uintptr_t) root);
        fullPath += "\\" + path;

        QStringList entries = wr.list(&err);
        for (int i = 0; i < entries.count(); i++) {
            WindowsRegistry k;
            err = k.open(wr, entries.at(i), KEY_READ);
            if (err.isEmpty()) {
                detectOneControlPanelProgram(fullPath + "\\" + entries.at(i),
                        k, entries.at(i), packagePaths, foundDetectionInfos);
            }
        }
    }
}

void InstalledPackages::detectOneControlPanelProgram(const QString& registryPath,
        WindowsRegistry& k,
        const QString& keyName, QStringList* packagePaths,
        QStringList* foundDetectionInfos)
{
    QString package = keyName;
    package.replace('.', '_');
    package = WPMUtils::makeValidFullPackageName(
            "control-panel." + package);

    bool versionFound = false;
    Version version;
    QString err;
    QString version_ = k.get("DisplayVersion", &err);
    if (err.isEmpty()) {
        version.setVersion(version_);
        version.normalize();
        versionFound = true;
    }
    if (!versionFound) {
        DWORD major = k.getDWORD("VersionMajor", &err);
        if (err.isEmpty()) {
            DWORD minor = k.getDWORD("VersionMinor", &err);
            if (err.isEmpty())
                version.setVersion(major, minor);
            else
                version.setVersion(major, 0);
            version.normalize();
            versionFound = true;
        }
    }
    if (!versionFound) {
        QString major = k.get("VersionMajor", &err);
        if (err.isEmpty()) {
            QString minor = k.get("VersionMinor", &err);
            if (err.isEmpty()) {
                if (version.setVersion(major)) {
                    versionFound = true;
                    version.normalize();
                }
            } else {
                if (version.setVersion(major + "." + minor)) {
                    versionFound = true;
                    version.normalize();
                }
            }
        }
    }
    if (!versionFound) {
        QString displayName = k.get("DisplayName", &err);
        if (err.isEmpty()) {
            QStringList parts = displayName.split(' ');
            if (parts.count() > 1 && parts.last().contains('.')) {
                version.setVersion(parts.last());
                version.normalize();
                versionFound = true;
            }
        }
    }


    //qDebug() << "InstalledPackages::detectOneControlPanelProgram.0";

    AbstractRepository* rep = AbstractRepository::getDefault_();

    PackageVersion* pv = rep->findPackageVersion_(package, version);
    if (!pv) {
        pv = new PackageVersion(package);
        pv->version = version;
        rep->savePackageVersion(pv);
    }
    delete pv;

    InstalledPackageVersion* ipv = this->findOrCreate(package, version);
    QString di = "control-panel:" + registryPath;
    ipv->setDetectionInfo(di);
    foundDetectionInfos->append(di);

    Package* p = rep->findPackage_(package);
    if (!p) {
        p = new Package(package, package);
    }

    QString title = k.get("DisplayName", &err);
    if (!err.isEmpty() || title.isEmpty())
        title = keyName;
    p->title = title;
    p->description = "[Control Panel] " + p->title;

    QString url = k.get("URLInfoAbout", &err);
    if (!err.isEmpty() || url.isEmpty() || !QUrl(url).isValid())
        url = "";
    if (url.isEmpty())
        url = k.get("URLUpdateInfo", &err);
    if (!err.isEmpty() || url.isEmpty() || !QUrl(url).isValid())
        url = "";
    p->url = url;

    QDir d;

    bool useThisEntry = !ipv->installed();

    QString uninstall;
    if (useThisEntry) {
        uninstall = k.get("QuietUninstallString", &err);
        if (!err.isEmpty())
            uninstall = "";
        if (uninstall.isEmpty())
            uninstall = k.get("UninstallString", &err);
        if (!err.isEmpty())
            uninstall = "";

        // some programs store in UninstallString the complete path to
        // the uninstallation program with spaces
        if (!uninstall.isEmpty() && uninstall.contains(" ") &&
                !uninstall.contains("\"") &&
                d.exists(uninstall))
            uninstall = "\"" + uninstall + "\"";

        if (uninstall.trimmed().isEmpty())
            useThisEntry = false;
    }

    // already detected as an MSI package
    if (uninstall.length() == 14 + 38 &&
            (uninstall.indexOf("MsiExec.exe /X", 0, Qt::CaseInsensitive) == 0 ||
            uninstall.indexOf("MsiExec.exe /I", 0, Qt::CaseInsensitive) == 0) &&
            WPMUtils::validateGUID(uninstall.right(38)) == "") {
        useThisEntry = false;
    }

    QString dir;
    if (useThisEntry) {
        dir = k.get("InstallLocation", &err);
        if (!err.isEmpty())
            dir = "";

        if (dir.isEmpty() && !uninstall.isEmpty()) {
            QStringList params = WPMUtils::parseCommandLine(uninstall, &err);
            if (err.isEmpty() && params.count() > 0 && d.exists(params[0])) {
                dir = WPMUtils::parentDirectory(params[0]);
            } /* DEBUG else {
                qDebug() << "cannot parse " << uninstall << " " << err <<
                        " " << params.count();
                if (params.count() > 0)
                    qDebug() << "cannot parse2 " << params[0] << " " <<
                            d.exists(params[0]);
            }*/
        }
    }

    if (useThisEntry) {
        if (!dir.isEmpty()) {
            dir = WPMUtils::normalizePath(dir);
            if (WPMUtils::isUnderOrEquals(dir, *packagePaths))
                useThisEntry = false;
        }
    }

    if (useThisEntry) {
        if (dir.isEmpty()) {
            dir = WPMUtils::getInstallationDirectory() +
                    "\\NpackdDetected\\" +
            WPMUtils::makeValidFilename(p->title, '_');
            if (d.exists(dir)) {
                dir = WPMUtils::findNonExistingFile(dir + "-" +
                        version.getVersionString() + "%1");
            }
            d.mkpath(dir);
        }

        if (d.exists(dir)) {
            if (d.mkpath(dir + "\\.Npackd")) {
                QFile file(dir + "\\.Npackd\\Uninstall.bat");
                if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                    QTextStream stream(&file);
                    stream.setCodec("UTF-8");
                    QString txt = uninstall + "\r\n";

                    stream << txt;
                    file.close();

                    //qDebug() << "InstalledPackages::detectOneControlPanelProgram "
                    //        "setting path for " << pv->toString() << " to" << dir;
                    ipv->setPath(dir);
                }
            }
            packagePaths->append(dir);
        }
    }

    // TODO: no error handling
    rep->savePackage(p);
    delete p;
}

void InstalledPackages::detectMSIProducts()
{
    QStringList all = WPMUtils::findInstalledMSIProducts();
    // qDebug() << all.at(0);

    QStringList packagePaths = getAllInstalledPackagePaths();

    AbstractRepository* rep = AbstractRepository::getDefault_();
    for (int i = 0; i < all.count(); i++) {
        QString guid = all.at(i);

        PackageVersion* pv = rep->findPackageVersionByMSIGUID_(guid);
        if (!pv) {
            QString package = "msi." + guid.mid(1, 36);

            QString err;
            QString version_ = WPMUtils::getMSIProductAttribute(guid,
                    INSTALLPROPERTY_VERSIONSTRING, &err);
            Version version;
            if (err.isEmpty()) {
                if (!version.setVersion(version_))
                    version.setVersion(1, 0);
                else
                    version.normalize();
            }

            pv = rep->findPackageVersion_(package, version);
            if (!pv) {
                rep->addPackageVersion(package, version);
                pv = new PackageVersion(package);
                pv->version = version;
            }
        }

        Package* p = rep->findPackage_(pv->package);
        if (!p) {
            QString err;
            QString title = WPMUtils::getMSIProductName(guid, &err);
            if (!err.isEmpty())
                title = guid;

            p = new Package(pv->package, title);
            p->description = "[MSI database] " + p->title + " GUID: " + guid;
        }

        if (p->url.isEmpty()) {
            QString err;
            QString url = WPMUtils::getMSIProductAttribute(guid,
                    INSTALLPROPERTY_URLINFOABOUT, &err);
            if (err.isEmpty() && QUrl(url).isValid())
                p->url = url;
        }

        if (p->url.isEmpty()) {
            QString err;
            QString url = WPMUtils::getMSIProductAttribute(guid,
                    INSTALLPROPERTY_HELPLINK, &err);
            if (err.isEmpty() && QUrl(url).isValid())
                p->url = url;
        }

        if (!pv->installed()) {
            QDir d;
            QString err;
            QString dir = WPMUtils::getMSIProductLocation(guid, &err);
            if (!err.isEmpty())
                dir = "";

            if (!dir.isEmpty()) {
                dir = WPMUtils::normalizePath(dir);
                if (WPMUtils::isUnderOrEquals(dir, packagePaths))
                    dir = "";
            }

            InstalledPackageVersion* ipv = this->findOrCreate(pv->package,
                    pv->version);
            ipv->setDetectionInfo("msi:" + guid);
            if (dir.isEmpty() || !d.exists(dir)) {
                dir = WPMUtils::getInstallationDirectory() +
                        "\\NpackdDetected\\" +
                WPMUtils::makeValidFilename(p->title, '_');
                if (d.exists(dir)) {
                    dir = WPMUtils::findNonExistingFile(dir + "-" +
                            pv->version.getVersionString() + "%1");
                }
                d.mkpath(dir);
            }

            if (d.exists(dir)) {
                if (d.mkpath(dir + "\\.Npackd")) {
                    QFile file(dir + "\\.Npackd\\Uninstall.bat");
                    if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                        QTextStream stream(&file);
                        stream.setCodec("UTF-8");
                        QString txt = "msiexec.exe /qn /norestart /Lime "
                                ".Npackd\\UninstallMSI.log /x" + guid + "\r\n" +
                                "set err=%errorlevel%" + "\r\n" +
                                "type .Npackd\\UninstallMSI.log" + "\r\n" +
                                "rem 3010=restart required" + "\r\n" +
                                "if %err% equ 3010 exit 0" + "\r\n" +
                                "if %err% neq 0 exit %err%" + "\r\n";

                        stream << txt;
                        file.close();
                        pv->setPath(dir);
                    }
                }
            }
        }

        rep->savePackage(p);
        delete pv;
        delete p;
    }

    // remove uninstalled MSI packages
    QMapIterator<QString, InstalledPackageVersion*> i(this->data);
    while (i.hasNext()) {
        i.next();
        InstalledPackageVersion* ipv = i.value();
        if (ipv->detectionInfo.length() == 4 + 38 &&
                ipv->detectionInfo.left(4) == "msi:" &&
                ipv->installed() &&
                !all.contains(ipv->detectionInfo.right(38))) {
            // DEBUG qDebug() << "uninstall " << pv->package << " " <<
            // DEBUG         pv->version.getVersionString();
            ipv->setPath("");
        }
    }
}

void InstalledPackages::detectDotNet()
{
    // http://stackoverflow.com/questions/199080/how-to-detect-what-net-framework-versions-and-service-packs-are-installed

    WindowsRegistry wr;
    QString err = wr.open(HKEY_LOCAL_MACHINE,
            "Software\\Microsoft\\NET Framework Setup\\NDP", false, KEY_READ);
    if (err.isEmpty()) {
        QStringList entries = wr.list(&err);
        if (err.isEmpty()) {
            for (int i = 0; i < entries.count(); i++) {
                QString v_ = entries.at(i);
                Version v;
                if (v_.startsWith("v") && v.setVersion(
                        v_.right(v_.length() - 1))) {
                    WindowsRegistry r;
                    err = r.open(wr, v_, KEY_READ);
                    if (err.isEmpty())
                        detectOneDotNet(r, v_);
                }
            }
        }
    }
}

void InstalledPackages::detectMicrosoftInstaller()
{
    Version v = WPMUtils::getDLLVersion("MSI.dll");
    Version nullNull(0, 0);
    if (v.compare(nullNull) > 0) {
        AbstractRepository* rep = AbstractRepository::getDefault_();
        rep->addPackageVersion("com.microsoft.WindowsInstaller", v);
        InstalledPackageVersion* ipv = findOrCreate(
                "com.microsoft.WindowsInstaller", v);
        if (!ipv->installed()) {
            ipv->setPath(WPMUtils::getWindowsDir());
        }
    }
}

void InstalledPackages::detectMSXML()
{
    Version v = WPMUtils::getDLLVersion("msxml.dll");
    Version nullNull(0, 0);
    if (v.compare(nullNull) > 0) {
        setPackageVersionPathIfNotInstalled("com.microsoft.MSXML", v,
                WPMUtils::getWindowsDir());
    }
    v = WPMUtils::getDLLVersion("msxml2.dll");
    if (v.compare(nullNull) > 0) {
        setPackageVersionPathIfNotInstalled("com.microsoft.MSXML", v,
                WPMUtils::getWindowsDir());
    }
    v = WPMUtils::getDLLVersion("msxml3.dll");
    if (v.compare(nullNull) > 0) {
        v.prepend(3);
        setPackageVersionPathIfNotInstalled("com.microsoft.MSXML", v,
                WPMUtils::getWindowsDir());
    }
    v = WPMUtils::getDLLVersion("msxml4.dll");
    if (v.compare(nullNull) > 0) {
        setPackageVersionPathIfNotInstalled("com.microsoft.MSXML", v,
                WPMUtils::getWindowsDir());
    }
    v = WPMUtils::getDLLVersion("msxml5.dll");
    if (v.compare(nullNull) > 0) {
        setPackageVersionPathIfNotInstalled("com.microsoft.MSXML", v,
                WPMUtils::getWindowsDir());
    }
    v = WPMUtils::getDLLVersion("msxml6.dll");
    if (v.compare(nullNull) > 0) {
        setPackageVersionPathIfNotInstalled("com.microsoft.MSXML", v,
                WPMUtils::getWindowsDir());
    }
}

void InstalledPackages::scanPre1_15Dir(bool exact)
{
    QDir aDir(WPMUtils::getInstallationDirectory());
    if (!aDir.exists())
        return;

    WindowsRegistry machineWR(HKEY_LOCAL_MACHINE, false);
    QString err;
    WindowsRegistry packagesWR = machineWR.createSubKey(
            "SOFTWARE\\Npackd\\Npackd\\Packages", &err);
    if (!err.isEmpty())
        return;

    QFileInfoList entries = aDir.entryInfoList(
            QDir::NoDotAndDotDot | QDir::Dirs);
    int count = entries.size();
    QString dirPath = aDir.absolutePath();
    dirPath.replace('/', '\\');
    for (int idx = 0; idx < count; idx++) {
        QFileInfo entryInfo = entries[idx];
        QString name = entryInfo.fileName();
        int pos = name.lastIndexOf("-");
        if (pos > 0) {
            QString packageName = name.left(pos);
            QString versionName = name.right(name.length() - pos - 1);

            if (Package::isValidName(packageName)) {
                Version version;
                if (version.setVersion(versionName)) {
                    AbstractRepository* rep = AbstractRepository::getDefault_();
                    Package* p = rep->findPackage_(packageName);
                    if (!exact || p) {
                        // using getVersionString() here to fix a bug in earlier
                        // versions where version numbers were not normalized
                        WindowsRegistry wr = packagesWR.createSubKey(
                                packageName + "-" + version.getVersionString(),
                                &err);
                        if (err.isEmpty()) {
                            wr.set("Path", dirPath + "\\" +
                                    name);
                            wr.setDWORD("External", 0);
                        }
                    }
                    delete p;
                }
            }
        }
    }
}

void InstalledPackages::detectPre_1_15_Packages()
{
    QString regPath = "SOFTWARE\\Npackd\\Npackd";
    WindowsRegistry machineWR(HKEY_LOCAL_MACHINE, false);
    QString err;
    WindowsRegistry npackdWR = machineWR.createSubKey(regPath, &err);
    if (err.isEmpty()) {
        DWORD b = npackdWR.getDWORD("Pre1_15DirScanned", &err);
        if (!err.isEmpty() || b != 1) {
            // store the references to packages in the old format (< 1.15)
            // in the registry
            scanPre1_15Dir(false);
            npackdWR.setDWORD("Pre1_15DirScanned", 1);
        }
    }
}


