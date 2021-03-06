/****************************************************************************
 **
 ** Copyright (C) 2013-2014 Jolla Ltd.
 ** Contact: Chris Adams <chris.adams@jollamobile.com>
 **
 ** This program/library is free software; you can redistribute it and/or
 ** modify it under the terms of the GNU Lesser General Public License
 ** version 2.1 as published by the Free Software Foundation.
 **
 ** This program/library is distributed in the hope that it will be useful,
 ** but WITHOUT ANY WARRANTY; without even the implied warranty of
 ** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 ** Lesser General Public License for more details.
 **
 ** You should have received a copy of the GNU Lesser General Public
 ** License along with this program/library; if not, write to the Free
 ** Software Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 ** 02110-1301 USA
 **
 ****************************************************************************/

#ifndef FACEBOOKSIGNONSYNCADAPTOR_H
#define FACEBOOKSIGNONSYNCADAPTOR_H

#include "facebookdatatypesyncadaptor.h"

#include <QtCore/QObject>
#include <QtCore/QString>
#include <QtCore/QDateTime>
#include <QtCore/QVariantMap>
#include <QtCore/QList>
#include <QtNetwork/QNetworkReply>
#include <QtNetwork/QSslError>

#include <Accounts/Manager>
#include <Accounts/Account>
#include <Accounts/AccountService>
#include <SignOn/Identity>
#include <SignOn/AuthSession>
#include <SignOn/SessionData>
#include <SignOn/Error>

class FacebookSignonSyncAdaptor : public FacebookDataTypeSyncAdaptor
{
    Q_OBJECT

public:
    FacebookSignonSyncAdaptor(QObject *parent);
    ~FacebookSignonSyncAdaptor();

    QString syncServiceName() const;
    void sync(const QString &dataTypeString, int accountId);

protected: // implementing FacebookDataTypeSyncAdaptor interface
    void purgeDataForOldAccount(int oldId, SocialNetworkSyncAdaptor::PurgeMode mode);
    void beginSync(int accountId, const QString &accessToken);
    void finalize(int accountId);

private Q_SLOTS:
    void requestFinishedHandler();
    void forceTokenExpiryResponse(const SignOn::SessionData &responseData);
    void forceTokenExpiryError(const SignOn::Error &error);

private:
    Accounts::Account *loadAccount(int accountId);
    void raiseCredentialsNeedUpdateFlag(int accountId);
    void lowerCredentialsNeedUpdateFlag(int accountId);
    void forceTokenExpiry(int seconds, int accountId, const QString &accessToken);

    Accounts::Manager m_accountManager;
    QMap<int, Accounts::Account *> m_accounts;
};

#endif // FACEBOOKSIGNONSYNCADAPTOR_H
