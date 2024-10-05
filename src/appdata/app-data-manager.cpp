/*
 * Copyright (C) 2023, LingmoSoft Co., Ltd.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 */

#include "app-data-manager.h"
#include "user-config.h"
#include "settings.h"

#include <application-info.h>
#include <QDebug>

#define APP_ICON_PREFIX "image://appicon/"

namespace LingmoMenu {

class AppDataWorker : public QObject
{
    Q_OBJECT
public:
    explicit AppDataWorker(AppDataManager *appManager = nullptr);

Q_SIGNALS:
    void appAdded(QList<DataEntity> apps);
    void appUpdated(QList<DataEntity> apps, bool totalUpdate);
    void appDeleted(QStringList idList);
    void favoriteAppChanged();
    void appDataBaseOpenFailed();

private Q_SLOTS:
    void initAppData();
    void onAppAdded(const QStringList &infos);
    void onAppUpdated(const LingmoSearch::ApplicationInfoMap &infos);
    void onAppUpdatedAll(const QStringList &infos);
    void onAppDeleted(QStringList infos);

public Q_SLOTS:
    void fixToFavoriteSlot(const QString &path, const int &num);
    void changedFavoriteOrderSlot(const QString &path, const int &num);
    void fixToTopSlot(const QString &path, const int &num);
    void setAppLaunched(const QString &path);

private:
    void updateFavoriteApps();
    void removeApps(QStringList& appIdList, QStringList &removedIdList);
    bool updateApps(const LingmoSearch::ApplicationInfoMap &infos, QList<DataEntity> &apps);
    void updateAppsAll(const QStringList &infos, QList<DataEntity> &apps);
    void appendApps(const QStringList &infos, QList<DataEntity> &apps);
    void addInfoToApp(const LingmoSearch::ApplicationPropertyMap &info, DataEntity &app);

private:
    AppDataManager *m_appManager{nullptr};
    LingmoSearch::ApplicationInfo *m_applicationInfo{nullptr};
    LingmoSearch::ApplicationProperties m_appProperties;
    LingmoSearch::ApplicationPropertyMap m_appPropertyMap;
};

AppDataWorker::AppDataWorker(AppDataManager *appManager) : QObject(nullptr), m_appManager(appManager)
{
    qRegisterMetaType<QList<DataEntity> >("QList<DataEntity>");
    qRegisterMetaType<QVector<DataEntity> >("QVector<DataEntity>");
    m_applicationInfo = new LingmoSearch::ApplicationInfo(this);
    if (!m_applicationInfo || !m_appManager) {
        return;
    }

    initAppData();

    connect(m_applicationInfo, &LingmoSearch::ApplicationInfo::appDBItems2BAdd, this, &AppDataWorker::onAppAdded);
    connect(m_applicationInfo, &LingmoSearch::ApplicationInfo::appDBItems2BUpdate, this, &AppDataWorker::onAppUpdated);
    connect(m_applicationInfo, &LingmoSearch::ApplicationInfo::appDBItems2BUpdateAll, this, &AppDataWorker::onAppUpdatedAll);
    connect(m_applicationInfo, &LingmoSearch::ApplicationInfo::appDBItems2BDelete, this, &AppDataWorker::onAppDeleted);
    connect(m_applicationInfo, &LingmoSearch::ApplicationInfo::DBOpenFailed, this, &AppDataWorker::appDataBaseOpenFailed);
}

void AppDataWorker::initAppData()
{
    if (UserConfig::instance()->isFirstStartUp()) {
        // 默认收藏应用
        for (const auto &appid : GlobalSetting::instance()->defaultFavoriteApps()) {
            m_applicationInfo->setAppToFavorites(appid);
        }
    }

    m_appProperties << LingmoSearch::ApplicationProperty::Property::Top
                          << LingmoSearch::ApplicationProperty::Property::Lock
                          << LingmoSearch::ApplicationProperty::Property::Favorites
                          << LingmoSearch::ApplicationProperty::Property::LaunchTimes
                          << LingmoSearch::ApplicationProperty::Property::DesktopFilePath
                          << LingmoSearch::ApplicationProperty::Property::Icon
                          << LingmoSearch::ApplicationProperty::Property::LocalName
                          << LingmoSearch::ApplicationProperty::Property::Category
                          << LingmoSearch::ApplicationProperty::Property::FirstLetterAll
                          << LingmoSearch::ApplicationProperty::Property::DontDisplay
                          << LingmoSearch::ApplicationProperty::Property::AutoStart
                          << LingmoSearch::ApplicationProperty::Property::InsertTime
                          << LingmoSearch::ApplicationProperty::Property::Launched;
    m_appPropertyMap.insert(LingmoSearch::ApplicationProperty::Property::DontDisplay, 0);
    m_appPropertyMap.insert(LingmoSearch::ApplicationProperty::Property::AutoStart, 0);

    LingmoSearch::ApplicationInfoMap appInfos = m_applicationInfo->getInfo(m_appProperties, m_appPropertyMap);

    if (appInfos.isEmpty()) {
        return;
    }

    for (const auto &info : appInfos) {
        DataEntity app;
        addInfoToApp(info, app);
        m_appManager->m_normalApps.insert(app.id(), app);
    }

    updateFavoriteApps();
}

void AppDataWorker::updateFavoriteApps()
{
    QVector<DataEntity> favoriteApps;
    for (const auto &app : m_appManager->m_normalApps) {
        if (app.favorite() > 0) {
            favoriteApps.append(app);
        }
    }

    if (!favoriteApps.isEmpty()) {
        // 排序搜藏夹数据
        std::sort(favoriteApps.begin(), favoriteApps.end(), [](const DataEntity& a, const DataEntity &b) {
            return a.favorite() < b.favorite();
        });
    }

    {
        QMutexLocker locker(&m_appManager->m_mutex);
        m_appManager->m_favoriteApps.swap(favoriteApps);
    }

    Q_EMIT favoriteAppChanged();
}

void AppDataWorker::onAppAdded(const QStringList &infos)
{
    if (infos.isEmpty()) {
        return;
    }

    QList<DataEntity> apps;
    appendApps(infos, apps);
    if (apps.isEmpty()) {
        return;
    }
    Q_EMIT appAdded(apps);

    updateFavoriteApps();
}

void AppDataWorker::appendApps(const QStringList &infos, QList<DataEntity> &apps)
{
    QMutexLocker locker(&m_appManager->m_mutex);
    for (const QString &info : infos) {
        const LingmoSearch::ApplicationPropertyMap appInfo = m_applicationInfo->getInfo(info, m_appProperties);
        if (appInfo.value(LingmoSearch::ApplicationProperty::Property::DontDisplay).toInt() != 0) {
            continue;
        }

        if (appInfo.value(LingmoSearch::ApplicationProperty::Property::AutoStart).toInt() != 0) {
            continue;
        }

        if (m_appManager->m_normalApps.contains(info)) {
            continue;
        }
        DataEntity app;
        addInfoToApp(appInfo, app);
        m_appManager->m_normalApps.insert(app.id(), app);
        apps.append(app);
    }
}

void AppDataWorker::addInfoToApp(const LingmoSearch::ApplicationPropertyMap &info, DataEntity &app)
{
    app.setTop(info.value(LingmoSearch::ApplicationProperty::Property::Top).toInt());
    app.setLock(info.value(LingmoSearch::ApplicationProperty::Property::Lock).toInt() == 1);
    app.setFavorite(info.value(LingmoSearch::ApplicationProperty::Property::Favorites).toInt());
    app.setLaunchTimes(info.value(LingmoSearch::ApplicationProperty::Property::LaunchTimes).toInt());
    app.setId(info.value(LingmoSearch::ApplicationProperty::Property::DesktopFilePath).toString());
    app.setIcon(APP_ICON_PREFIX + info.value(LingmoSearch::ApplicationProperty::Property::Icon).toString());
    app.setName(info.value(LingmoSearch::ApplicationProperty::Property::LocalName).toString());
    app.setCategory(info.value(LingmoSearch::ApplicationProperty::Property::Category).toString());
    app.setFirstLetter(info.value(LingmoSearch::ApplicationProperty::Property::FirstLetterAll).toString());
    app.setInsertTime(info.value(LingmoSearch::ApplicationProperty::Property::InsertTime).toString());
    app.setLaunched(info.value(LingmoSearch::ApplicationProperty::Property::Launched).toInt());
}

void AppDataWorker::onAppUpdated(const LingmoSearch::ApplicationInfoMap &infos)
{
    if (infos.isEmpty()) {
        return;
    }

    QList<DataEntity> apps;
    bool totalUpdate = updateApps(infos, apps);
    if (apps.isEmpty()) {
        return;
    }
    Q_EMIT appUpdated(apps, totalUpdate);

    updateFavoriteApps();
}

void AppDataWorker::onAppUpdatedAll(const QStringList &infos)
{
    if (infos.isEmpty()) {
        return;
    }

    QList<DataEntity> apps;
    updateAppsAll(infos, apps);
    if (apps.isEmpty()) {
        return;
    }
    Q_EMIT appUpdated(apps, true);

    updateFavoriteApps();
}

bool AppDataWorker::updateApps(const LingmoSearch::ApplicationInfoMap &infos, QList<DataEntity> &apps)
{
    QMutexLocker locker(&m_appManager->m_mutex);
    if (infos.isEmpty()) {
        return false;
    }
    bool totalUpdate = false;
    for (const QString &info : infos.keys()) {
        if (m_appManager->m_normalApps.contains(info)) {
            DataEntity &app = m_appManager->m_normalApps[info];
            for (auto &appProperty : infos.value(info).keys()) {
                switch (appProperty) {
                case LingmoSearch::ApplicationProperty::Property::Top:
                    app.setTop(infos.value(info).value(LingmoSearch::ApplicationProperty::Property::Top).toInt());
                    break;
                case LingmoSearch::ApplicationProperty::Property::Lock:
                    app.setLock(infos.value(info).value(LingmoSearch::ApplicationProperty::Property::Lock).toInt() == 1);
                    break;
                case LingmoSearch::ApplicationProperty::Property::Favorites:
                    app.setFavorite(infos.value(info).value(LingmoSearch::ApplicationProperty::Property::Favorites).toInt());
                    break;
                case LingmoSearch::ApplicationProperty::Property::LaunchTimes:
                    app.setLaunchTimes(infos.value(info).value(LingmoSearch::ApplicationProperty::Property::LaunchTimes).toInt());
                    break;
                case LingmoSearch::ApplicationProperty::Property::DesktopFilePath:
                    app.setId(infos.value(info).value(LingmoSearch::ApplicationProperty::Property::DesktopFilePath).toString());
                    break;
                case LingmoSearch::ApplicationProperty::Property::Icon:
                    app.setIcon(APP_ICON_PREFIX + infos.value(info).value(LingmoSearch::ApplicationProperty::Property::Icon).toString());
                    break;
                case LingmoSearch::ApplicationProperty::Property::LocalName:
                    app.setName(infos.value(info).value(LingmoSearch::ApplicationProperty::Property::LocalName).toString());
                    break;
                case LingmoSearch::ApplicationProperty::Property::Category:
                    app.setCategory(infos.value(info).value(LingmoSearch::ApplicationProperty::Property::Category).toString());
                    totalUpdate = true;
                    break;
                case LingmoSearch::ApplicationProperty::Property::FirstLetterAll:
                    app.setFirstLetter(infos.value(info).value(LingmoSearch::ApplicationProperty::Property::FirstLetterAll).toString());
                    totalUpdate = true;
                    break;
                case LingmoSearch::ApplicationProperty::Property::InsertTime:
                    app.setInsertTime(infos.value(info).value(LingmoSearch::ApplicationProperty::Property::InsertTime).toString());
                    break;
                case LingmoSearch::ApplicationProperty::Property::Launched:
                    app.setLaunched(infos.value(info).value(LingmoSearch::ApplicationProperty::Property::Launched).toInt());
                    break;
                default:
                    break;
                }
            }
            apps.append(app);
        }
    }
    return totalUpdate;
}

void AppDataWorker::updateAppsAll(const QStringList &infos, QList<DataEntity> &apps)
{
    QMutexLocker locker(&m_appManager->m_mutex);

    for (const auto &info : infos) {
        if (m_appManager->m_normalApps.contains(info)) {
            LingmoSearch::ApplicationPropertyMap appinfo = m_applicationInfo->getInfo(info, m_appProperties);
            DataEntity &app = m_appManager->m_normalApps[info];
            addInfoToApp(appinfo, app);
            apps.append(app);
        }
    }
}

void AppDataWorker::onAppDeleted(QStringList infos)
{
    if (infos.isEmpty()) {
        return;
    }
    QStringList removedIdList;
    removeApps(infos, removedIdList);
    if (removedIdList.isEmpty()) {
        return;
    }
    Q_EMIT appDeleted(removedIdList);

    updateFavoriteApps();

    for (const auto &appid : removedIdList) {
        UserConfig::instance()->removePreInstalledApp(appid);
    }
    UserConfig::instance()->sync();
}

void AppDataWorker::fixToFavoriteSlot(const QString &path, const int &num)
{
    if (num == 0) {
        m_applicationInfo->setFavoritesOfApp(path, 0);
    } else {
        m_applicationInfo->setFavoritesOfApp(path, m_appManager->m_favoriteApps.length() + 1);
    }
}

void AppDataWorker::changedFavoriteOrderSlot(const QString &path, const int &num)
{
    m_applicationInfo->setFavoritesOfApp(path, num);
}

void AppDataWorker::fixToTopSlot(const QString &path, const int &num)
{
    if (num == 0) {
        m_applicationInfo->setAppToTop(path);
    } else {
        m_applicationInfo->setTopOfApp(path, 0);
    }
}

void AppDataWorker::setAppLaunched(const QString &path)
{
    if (m_appManager->m_normalApps.value(path).launched() == 0) {
        m_applicationInfo->setAppLaunchedState(path);
    }
}

void AppDataWorker::removeApps(QStringList &appIdList, QStringList &removedIdList)
{
    QMutexLocker locker(&m_appManager->m_mutex);

    for (const QString &id : appIdList) {
        if (m_appManager->m_normalApps.remove(id)) {
            removedIdList.append(id);
        }
    }
}

// ===== AppDataManager ===== //
AppDataManager *AppDataManager::instance()
{
    static AppDataManager appDataManager;
    return &appDataManager;
}

AppDataManager::AppDataManager()
{
    AppDataWorker *appDataWorker = new AppDataWorker(this);
    appDataWorker->moveToThread(&m_workerThread);

    connect(&m_workerThread, &QThread::finished, appDataWorker, &QObject::deleteLater);
    connect(appDataWorker, &AppDataWorker::appAdded, this, &AppDataManager::appAdded);
    connect(appDataWorker, &AppDataWorker::appDeleted, this, &AppDataManager::appDeleted);
    connect(appDataWorker, &AppDataWorker::appUpdated, this, &AppDataManager::appUpdated);
    connect(appDataWorker, &AppDataWorker::favoriteAppChanged, this, &AppDataManager::favoriteAppChanged);
    connect(this, &AppDataManager::fixToFavoriteSignal, appDataWorker, &AppDataWorker::fixToFavoriteSlot);
    connect(this, &AppDataManager::changedFavoriteOrderSignal, appDataWorker, &AppDataWorker::changedFavoriteOrderSlot);
    connect(this, &AppDataManager::fixToTop, appDataWorker, &AppDataWorker::fixToTopSlot);
    connect(this, &AppDataManager::appLaunch, appDataWorker, &AppDataWorker::setAppLaunched);

    m_workerThread.start();
}

AppDataManager::~AppDataManager()
{
    m_workerThread.quit();
    m_workerThread.wait();
}

QList<DataEntity> AppDataManager::normalApps()
{
    QMutexLocker locker(&m_mutex);
    return m_normalApps.values();
}

QVector<DataEntity> AppDataManager::favoriteApps()
{
    QMutexLocker locker(&m_mutex);
    return m_favoriteApps;
}

bool AppDataManager::getApp(const QString &appId, DataEntity &app)
{
    QMutexLocker locker(&m_mutex);
    if (!m_normalApps.contains(appId)) {
        return false;
    }

    app = m_normalApps[appId];
    return true;
}

} // LingmoMenu

#include "app-data-manager.moc"
