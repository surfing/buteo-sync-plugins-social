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

#include "networkstubs_p.h"
#include "socialdnetworkaccessmanager_p.h"

#include <QDateTime>
#include <QTimer>

/*
 *  This implementation overrides the networking classes for testing purposes.
 *  The unit test for each social network should provide an implementation of
 *  TestNetworkReply::generateData() in order to return the required data.
 */

SocialdNetworkAccessManager::SocialdNetworkAccessManager(QObject *parent)
    : QNetworkAccessManager(parent)
{
}

QNetworkReply *SocialdNetworkAccessManager::createRequest(QNetworkAccessManager::Operation op,
                                                          const QNetworkRequest &req,
                                                          QIODevice *outgoingData)
{
    Q_UNUSED(op)
    Q_UNUSED(outgoingData)

    QString generator; // TODO: create generator from request, if we want to test different branches.

    // construct a test reply, which will contain the expected data and trigger finished()
    TestNetworkReply *retn = new TestNetworkReply(this);
    retn->setOperation(op);
    retn->setRequest(req);
    retn->setUrl(req.url());
    retn->m_readAllData = TestNetworkReply::generateData(req.url(), generator);
    retn->open(QIODevice::ReadOnly);
    retn->setFinished(true);
    QTimer::singleShot(100, retn, SIGNAL(finished()));
    return retn;
}
