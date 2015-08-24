/****************************************************************************
 **
 ** Copyright (C) 2013 Jolla Ltd.
 ** Contact: Bea Lam <bea.lam@jollamobile.com>
 **
 ****************************************************************************/

#include "vkpostsyncadaptor.h"
#include "trace.h"
#include "constants_p.h"

#include <QtCore/QPair>
#include <QtCore/QJsonArray>
#include <QtCore/QJsonValue>
#include <QtCore/QJsonObject>
#include <QtCore/QJsonDocument>
#include <QtCore/QUrlQuery>

#include <MGConfItem>

#define SOCIALD_VK_POSTS_ID_PREFIX QStringLiteral("vk-posts-")
#define SOCIALD_VK_POSTS_GROUPNAME QStringLiteral("vk")

VKPostSyncAdaptor::VKPostSyncAdaptor(QObject *parent)
    : VKDataTypeSyncAdaptor(SocialNetworkSyncAdaptor::Posts, parent)
{
    setInitialActive(m_db.isValid());
}

VKPostSyncAdaptor::~VKPostSyncAdaptor()
{
}

QString VKPostSyncAdaptor::syncServiceName() const
{
    return QStringLiteral("vk-microblog");
}

void VKPostSyncAdaptor::sync(const QString &dataTypeString, int accountId)
{
    // call superclass impl.
    VKDataTypeSyncAdaptor::sync(dataTypeString, accountId);
}

void VKPostSyncAdaptor::purgeDataForOldAccount(int oldId, SocialNetworkSyncAdaptor::PurgeMode)
{
    m_db.removePosts(oldId);
    m_db.commit();
    m_db.wait();

    // social media feed UI caches feed images and maintains bindings between
    // source and cached image in SocialImageDatabase.
    // purge cached images belonging to this account.
    purgeCachedImages(&m_imageCacheDb, oldId);
}

void VKPostSyncAdaptor::beginSync(int accountId, const QString &accessToken)
{
    SOCIALD_LOG_DEBUG("beginning VK posts sync with account:" << accountId);
    requestPosts(accountId, accessToken);
}

void VKPostSyncAdaptor::finalize(int accountId)
{
    if (syncAborted()) {
        SOCIALD_LOG_DEBUG("sync aborted, skipping finalize of VK Posts from account:" << accountId);
    } else {
        SOCIALD_LOG_DEBUG("finalizing VK posts sync with account:" << accountId);
        determineOptimalImageSize();
        Q_FOREACH (const PostData &post, m_postsToAdd) {
            saveVKPostFromObject(post.accountId, post.post, post.userProfiles, post.groupProfiles);
        }
        m_db.commit();
        m_db.wait();

        // manage image cache. Social media feed UI caches feed images
        // and maintains bindings between source and cached image in SocialImageDatabase.
        // purge cached images older than four weeks.
        purgeExpiredImages(&m_imageCacheDb, accountId);
        setLastSuccessfulSyncTime(accountId);
    }
}

void VKPostSyncAdaptor::requestPosts(int accountId, const QString &accessToken)
{
    QDateTime since = lastSuccessfulSyncTime(accountId);
    if (!since.isValid()) {
        int sinceSpan = m_accountSyncProfile
                      ? m_accountSyncProfile->key(Buteo::KEY_SYNC_SINCE_DAYS_PAST, QStringLiteral("7")).toInt()
                      : 7;
        since = QDateTime::currentDateTime().addDays(-1 * sinceSpan).toUTC();
    }

    QList<QPair<QString, QString> > queryItems;
    queryItems.append(QPair<QString, QString>(QStringLiteral("access_token"), accessToken));
    queryItems.append(QPair<QString, QString>(QStringLiteral("extended"), QStringLiteral("1")));
    queryItems.append(QPair<QString, QString>(QStringLiteral("v"), QStringLiteral("5.21"))); // version
    queryItems.append(QPair<QString, QString>(QStringLiteral("filters"), QStringLiteral("post,photo,photo_tag,wall_photo,note")));
    //queryItems.append(QPair<QString, QString>(QStringLiteral("start_time"), QString::number(since.toTime_t())));

    QUrl url(QStringLiteral("https://api.vk.com/method/newsfeed.get"));
    QUrlQuery query(url);
    query.setQueryItems(queryItems);
    url.setQuery(query);
    QNetworkReply *reply = m_networkAccessManager->get(QNetworkRequest(url));
    
    if (reply) {
        reply->setProperty("accountId", accountId);
        reply->setProperty("accessToken", accessToken);
        connect(reply, SIGNAL(error(QNetworkReply::NetworkError)), this, SLOT(errorHandler(QNetworkReply::NetworkError)));
        connect(reply, SIGNAL(sslErrors(QList<QSslError>)), this, SLOT(sslErrorsHandler(QList<QSslError>)));
        connect(reply, SIGNAL(finished()), this, SLOT(finishedPostsHandler()));

        // we're requesting data.  Increment the semaphore so that we know we're still busy.
        incrementSemaphore(accountId);
        setupReplyTimeout(accountId, reply);
    } else {
        SOCIALD_LOG_ERROR("error: unable to request home posts from VK account with id:" << accountId);
    }
}

void VKPostSyncAdaptor::finishedPostsHandler()
{
    QNetworkReply *reply = qobject_cast<QNetworkReply*>(sender());
    bool isError = reply->property("isError").toBool();
    int accountId = reply->property("accountId").toInt();

    QByteArray replyData = reply->readAll();
    disconnect(reply);
    reply->deleteLater();
    removeReplyTimeout(accountId, reply);

    bool ok = false;
    QJsonObject parsed = parseJsonObjectReplyData(replyData, &ok);

    if (!isError && ok && parsed.contains(QStringLiteral("response"))) {
        QJsonObject responseObj = parsed.value(QStringLiteral("response")).toObject();
        QJsonArray items = responseObj.value(QStringLiteral("items")).toArray();
        if (!items.size()) {
            SOCIALD_LOG_DEBUG("no feed posts received for account:" << accountId);
            decrementSemaphore(accountId);
            return;
        }

        QJsonArray profileValues = responseObj.value(QStringLiteral("profiles")).toArray();
        QList<UserProfile> userProfiles;
        foreach (const QJsonValue &entry, profileValues) {
            UserProfile profile = UserProfile::fromJsonObject(entry.toObject());
            userProfiles << profile;
        }

        QJsonArray groupValues = responseObj.value(QStringLiteral("groups")).toArray();
        QList<GroupProfile> groupProfiles;
        foreach (const QJsonValue &entry, groupValues) {
            GroupProfile profile = GroupProfile::fromJsonObject(entry.toObject());
            groupProfiles << profile;
        }

        foreach (const QJsonValue &entry, items) {
            QJsonObject object = entry.toObject();
            if (!object.isEmpty()) {
                if (object.value(QStringLiteral("type")).toString() == QStringLiteral("post")) {
                    m_postsToAdd.append(PostData(accountId, object, userProfiles, groupProfiles));
                } else  if (object.value(QStringLiteral("type")).toString() == QStringLiteral("photo")) {
                    SOCIALD_LOG_DEBUG("TODO: unhandled newsfeed item type:" << object.value(QStringLiteral("type")).toString() << ", skipping.");
                } else if (object.value(QStringLiteral("type")).toString() == QStringLiteral("photo_tag")) {
                    SOCIALD_LOG_DEBUG("TODO: unhandled newsfeed item type:" << object.value(QStringLiteral("type")).toString() << ", skipping.");
                } else if (object.value(QStringLiteral("type")).toString() == QStringLiteral("wall_photo")) {
                    SOCIALD_LOG_DEBUG("TODO: unhandled newsfeed item type:" << object.value(QStringLiteral("type")).toString() << ", skipping.");
                } else if (object.value(QStringLiteral("type")).toString() == QStringLiteral("note")) {
                    SOCIALD_LOG_DEBUG("TODO: unhandled newsfeed item type:" << object.value(QStringLiteral("type")).toString() << ", skipping.");
                } else {
                    SOCIALD_LOG_DEBUG("unhandled newsfeed item type:" << object.value(QStringLiteral("type")).toString() << ", skipping.");
                }
            } else {
                SOCIALD_LOG_DEBUG("post object empty; skipping");
            }
        }
    } else {
        // error occurred during request.
        SOCIALD_LOG_ERROR("error: unable to parse event feed data from request with account"
                          << accountId << "got:" << QString::fromUtf8(replyData));
    }

    // we're finished this request.  Decrement our busy semaphore.
    decrementSemaphore(accountId);
}

void VKPostSyncAdaptor::saveVKPostFromObject(int accountId, const QJsonObject &post, const QList<UserProfile> &userProfiles, const QList<GroupProfile> &groupProfiles)
{
    VKPostsDatabase::Post newPost;
    newPost.fromId = post.contains(QStringLiteral("from_id"))
                   ? int(post.value(QStringLiteral("from_id")).toDouble())
                   : int(post.value(QStringLiteral("source_id")).toDouble());
    newPost.toId = int(post.value(QStringLiteral("to_id")).toDouble());
    newPost.postType = post.value(QStringLiteral("post_type")).toString();
    newPost.replyOwnerId = int(post.value(QStringLiteral("reply_owner_id")).toDouble());
    newPost.replyPostId = int(post.value(QStringLiteral("reply_post_id")).toDouble());
    newPost.signerId = int(post.value(QStringLiteral("signer_id")).toDouble());
    newPost.friendsOnly = int(post.value(QStringLiteral("friends_only")).toDouble()) == 1;

    QJsonObject commentInfo = post.value(QStringLiteral("comments")).toObject();
    VKPostsDatabase::Comments comments;
    comments.count = int(commentInfo.value(QStringLiteral("count")).toDouble());
    comments.userCanComment = int(commentInfo.value(QStringLiteral("count")).toDouble()) == 1;
    newPost.comments = comments;

    QJsonObject likeInfo = post.value(QStringLiteral("likes")).toObject();
    VKPostsDatabase::Likes likes;
    likes.count = int(likeInfo.value(QStringLiteral("count")).toDouble());
    likes.userLikes = int(likeInfo.value(QStringLiteral("user_likes")).toDouble()) == 1;
    likes.userCanLike = int(likeInfo.value(QStringLiteral("can_like")).toDouble()) == 1;
    likes.userCanPublish = int(likeInfo.value(QStringLiteral("can_publish")).toDouble()) == 1;
    newPost.likes = likes;

    QJsonObject repostInfo = post.value(QStringLiteral("reposts")).toObject();
    VKPostsDatabase::Reposts reposts;
    reposts.count = int(repostInfo.value(QStringLiteral("count")).toDouble());
    reposts.userReposted = int(repostInfo.value(QStringLiteral("user_reposted")).toDouble()) == 1;
    newPost.reposts = reposts;

    QJsonObject postSourceInfo = post.value(QStringLiteral("post_source")).toObject();
    VKPostsDatabase::PostSource postSource;
    postSource.type = postSourceInfo.value(QStringLiteral("type")).toString();
    postSource.data = postSourceInfo.value(QStringLiteral("data")).toString();
    newPost.postSource = postSource;

    QList<QPair<QString, SocialPostImage::ImageType> > images;
    QJsonArray attachmentsInfo = post.value(QStringLiteral("attachments")).toArray();
    Q_FOREACH (const QJsonValue &attValue, attachmentsInfo) {
        QJsonObject attObject = attValue.toObject();
        QString type = attObject.value(QStringLiteral("type")).toString();
        QJsonObject typedValue = attObject.value(type).toObject();
        if (type == QStringLiteral("photo")
                || type == QStringLiteral("posted_photo")
                || type == QStringLiteral("graffiti")) {
            QString src = typedValue.value(m_optimalImageSize).toString();
            if (!src.isEmpty()) {
                images.append(qMakePair(src, SocialPostImage::Photo));
            }
        }
    }

    VKPostsDatabase::GeoLocation geo;
    QJsonObject geoInfo = post.value(QStringLiteral("geo")).toObject();
    if (!geoInfo.isEmpty()) {
        geo.placeId = int(geoInfo.value(QStringLiteral("place_id")).toDouble());
        geo.title = geoInfo.value(QStringLiteral("title")).toString();
        geo.type = geoInfo.value(QStringLiteral("type")).toString();
        geo.countryId = int(geoInfo.value(QStringLiteral("country_id")).toDouble());
        geo.cityId = int(geoInfo.value(QStringLiteral("city_id")).toDouble());
        geo.address = geoInfo.value(QStringLiteral("address")).toString();
        geo.showMap = int(geoInfo.value(QStringLiteral("showmap")).toDouble()) == 1;   // type????
        newPost.geo = geo;
    }

    VKPostsDatabase::CopyPost copyPost;
/*    copyPost.createdTime = VKDataTypeSyncAdaptor::parseVKDateTime(post.value(QStringLiteral("copy_post_date")));
    copyPost.type = post.value(QStringLiteral("copy_post_type")).toString();
    copyPost.ownerId = int(post.value(QStringLiteral("copy_owner_id")).toDouble());
    copyPost.postId = int(post.value(QStringLiteral("copy_post_id")).toDouble());
    copyPost.text = post.value(QStringLiteral("copy_text")).toString(); */

    if (post.contains(QStringLiteral("copy_history")) && copyPost.type.isEmpty()) {
        QJsonArray copyHistory = post.value(QStringLiteral("copy_history")).toArray();
        //qDebug() << "COPY HISTORY KEYS: " << copyHistory.toVariantList();
        foreach (const QJsonValue historyValue, copyHistory) {
            QJsonObject object = historyValue.toObject();
            QStringList keys = object.keys();
            if (keys.contains(QStringLiteral("owner_id"))) {
                int ownerId = (int)object.value(QStringLiteral("owner_id")).toDouble();
                if (ownerId != 0) {
                    if (ownerId > 0) {
                        const UserProfile &user(VKDataTypeSyncAdaptor::findUserProfile(userProfiles, ownerId));
                        copyPost.ownerName = user.name();
                        copyPost.ownerAvatar = user.icon;
                        //qDebug() << "REPOST FROM USER: " << copyPost.ownerName << "   AVATAR: " << copyPost.ownerAvatar;
                    } else {
                        // it was posted by a group
                        const GroupProfile &group(VKDataTypeSyncAdaptor::findGroupProfile(groupProfiles, ownerId));
                        copyPost.ownerName = group.name;
                        copyPost.ownerAvatar = group.icon;
                        ownerId = -ownerId;
                        //qDebug() << "REPOST FROM GROUP: " << copyPost.ownerName << "   AVATAR: " << copyPost.ownerAvatar;
                    }

                    copyPost.ownerId = ownerId;
                    //qDebug("HAD OWNER ID: %d", copyPost.ownerId);

                    copyPost.createdTime = VKDataTypeSyncAdaptor::parseVKDateTime(object.value(QStringLiteral("date")));

                    if (keys.contains(QStringLiteral("attachments"))) {
                        QJsonArray attachments = object.value(QStringLiteral("attachments")).toArray();
                        foreach (const QJsonValue attachment, attachments) {
                            QJsonObject attachmentObject = attachment.toObject();
                            copyPost.type = attachmentObject.value("type").toString();
                            //qDebug() << "ATT: " << attachmentObject.keys();
                            //qDebug() << "ATT OBJ: " << attachmentObject.toVariantMap();
                            //qDebug("   TYPE: %s", qPrintable(attachmentObject.value("type").toString()));
                            if (copyPost.type == QStringLiteral("photo")) {
                                QJsonObject photoObject = attachmentObject.value(QStringLiteral("photo")).toObject();
                                copyPost.photo = photoObject.value(m_optimalImageSize).toString();
                                //qDebug("PARSED REPOST PHOTO: %s", qPrintable(copyPost.photo));
                            }
                        }
                    }
                    if (keys.contains(QStringLiteral("text"))) {
                        //qDebug("HAS TEXT");
                        copyPost.text = object.value(QStringLiteral("text")).toString();
                    }
                }
            }
        }

        if (copyPost.type.isEmpty()) {
            copyPost.type = QStringLiteral("post");
        }
        //qDebug("SET COPY POST TYPE TO: %s", qPrintable(copyPost.type));
        //qDebug("SET COPY POST TEXT TO: %s", qPrintable(copyPost.text));
    }

    newPost.copyPost = copyPost;

    QDateTime createdTime = VKDataTypeSyncAdaptor::parseVKDateTime(post.value(QStringLiteral("date")));
    QString body = post.value(QStringLiteral("text")).toString();
    QString posterName, posterIcon;
    int fromId = newPost.fromId;
    if (newPost.fromId < 0) {
        // it was posted by a group
        const GroupProfile &group(VKDataTypeSyncAdaptor::findGroupProfile(groupProfiles, newPost.fromId));
        posterName = group.name;
        posterIcon = group.icon;
        fromId = -fromId;
    } else {
        // it was posted by a user
        const UserProfile &user(VKDataTypeSyncAdaptor::findUserProfile(userProfiles, newPost.fromId));
        posterName = user.name();
        posterIcon = user.icon;
    }

    // VK post indentifier is just index number and not globally unique. To make
    // it unique we combine it with fromId
    QString identifier = QString::number(fromId) + QStringLiteral("_") +
                       (post.contains(QStringLiteral("id"))
                          ? QString::number(post.value(QStringLiteral("id")).toDouble())
                          : QString::number(post.value(QStringLiteral("post_id")).toDouble()));

    Q_FOREACH (const QString &line, body.split('\n')) { SOCIALD_LOG_TRACE(line); }

    m_db.addVKPost(identifier, createdTime, body, newPost, images, posterName, posterIcon, accountId);
}


void VKPostSyncAdaptor::determineOptimalImageSize()
{
    int width = 0, height = 0;
    const int defaultValue = 0;
    MGConfItem widthConf("/lipstick/screen/primary/width");
    if (widthConf.value(defaultValue).toInt() != defaultValue) {
        width = widthConf.value(defaultValue).toInt();
    }
    MGConfItem heightConf("/lipstick/screen/primary/height");
    if (heightConf.value(defaultValue).toInt() != defaultValue) {
        height = heightConf.value(defaultValue).toInt();
    }

    // we want to use the largest of these dimensions as the "optimal"
    int maxDimension = qMax(width, height);
    if (maxDimension >= 2048) {
        m_optimalImageSize = "photo_1280";
    } else if (maxDimension >= 960) {
        m_optimalImageSize = "photo_604";
    } else {
        m_optimalImageSize = "photo_75";
    }

    SOCIALD_LOG_DEBUG("Determined optimal image size for dimension " << maxDimension << " as " << m_optimalImageSize);
}

// TODO: this is also in Facebook notifications adapter. Move to base class.
QDateTime VKPostSyncAdaptor::lastSuccessfulSyncTime(int accountId)
{
    QDateTime result;
    QString settingsFileName = QString::fromLatin1("%1/%2/vkposts.ini")
            .arg(QString::fromLatin1(PRIVILEGED_DATA_DIR))
            .arg(QString::fromLatin1(SYNC_DATABASE_DIR));
    QSettings settingsFile(settingsFileName, QSettings::IniFormat);
    uint timestamp = settingsFile.value(QString::fromLatin1("%1-last-successful-sync-time").arg(accountId)).toUInt();
    if (timestamp > 0) {
        result = QDateTime::fromTime_t(timestamp);
    }
    return result;
}

void VKPostSyncAdaptor::setLastSuccessfulSyncTime(int accountId)
{
    QDateTime currentTime = QDateTime::currentDateTime().toUTC();
    QString settingsFileName = QString::fromLatin1("%1/%2/vkposts.ini")
            .arg(QString::fromLatin1(PRIVILEGED_DATA_DIR))
            .arg(QString::fromLatin1(SYNC_DATABASE_DIR));
    QSettings settingsFile(settingsFileName, QSettings::IniFormat);
    settingsFile.setValue(QString::fromLatin1("%1-last-successful-sync-time").arg(accountId),
                          QVariant::fromValue<uint>(currentTime.toTime_t()));
    settingsFile.sync();
}
