#include "providers/seventv/SeventvEmotes.hpp"

#include "Application.hpp"
#include "common/Literals.hpp"
#include "common/network/NetworkResult.hpp"
#include "common/QLogging.hpp"
#include "messages/Emote.hpp"
#include "messages/Image.hpp"
#include "messages/ImageSet.hpp"
#include "messages/MessageBuilder.hpp"
#include "providers/seventv/eventapi/Dispatch.hpp"
#include "providers/seventv/SeventvAPI.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "singletons/Settings.hpp"
#include "util/Helpers.hpp"

#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QThread>

#include <array>
#include <utility>

/**
 * # References
 *
 * - EmoteSet: https://github.com/SevenTV/API/blob/a84e884b5590dbb5d91a5c6b3548afabb228f385/data/model/emote-set.model.go#L8-L18
 * - ActiveEmote: https://github.com/SevenTV/API/blob/a84e884b5590dbb5d91a5c6b3548afabb228f385/data/model/emote-set.model.go#L20-L27
 * - EmotePartial (emoteData): https://github.com/SevenTV/API/blob/a84e884b5590dbb5d91a5c6b3548afabb228f385/data/model/emote.model.go#L24-L34
 * - ImageHost: https://github.com/SevenTV/API/blob/a84e884b5590dbb5d91a5c6b3548afabb228f385/data/model/model.go#L36-L39
 * - ImageFile: https://github.com/SevenTV/API/blob/a84e884b5590dbb5d91a5c6b3548afabb228f385/data/model/model.go#L41-L48
 */
namespace {

using namespace chatterino;
using namespace seventv::eventapi;

// These declarations won't throw an exception.
const QString CHANNEL_HAS_NO_EMOTES("This channel has no 7TV channel emotes.");
const QString EMOTE_LINK_FORMAT("https://7tv.app/emotes/%1");

// This is non-const, but only used on the GUI thread
// NOLINTNEXTLINE(cppcoreguidelines-avoid-non-const-global-variables)
auto ALLOW_AVIF_IMAGES = []() {
    static bool allow = true;
    static bool registered = false;
    if (!registered)
    {
        // We can't register this in the SeventvEmotes constructor,
        // so we register the handler on demand.
        getSettings()->allowAvifImages.connect([](bool setting) {
            allow = setting && QImageReader::supportedImageFormats().contains(
                                   QByteArrayLiteral("avif"));
        });
        registered = true;
    }
    return allow;
};

struct CreateEmoteResult {
    Emote emote;
    EmoteId id;
    EmoteName name;
    bool hasImages{};
};

EmotePtr cachedOrMake(Emote &&emote, const EmoteId &id)
{
    static std::unordered_map<EmoteId, std::weak_ptr<const Emote>> cache;
    static std::mutex mutex;

    return cachedOrMakeEmotePtr(std::move(emote), cache, mutex, id);
}

/**
  * This decides whether an emote should be displayed
  * as zero-width
  */
bool isZeroWidthActive(const QJsonObject &activeEmote)
{
    auto flags = SeventvActiveEmoteFlags(
        SeventvActiveEmoteFlag(activeEmote.value("flags").toInt()));
    return flags.has(SeventvActiveEmoteFlag::ZeroWidth);
}

/**
  * This is only an indicator if an emote should be added
  * as zero-width or not. The user can still overwrite this.
  */
bool isZeroWidthRecommended(const QJsonObject &emoteData)
{
    auto flags =
        SeventvEmoteFlags(SeventvEmoteFlag(emoteData.value("flags").toInt()));
    return flags.has(SeventvEmoteFlag::ZeroWidth);
}

QString kindToString(SeventvEmoteSetKind kind)
{
    switch (kind)
    {
        case SeventvEmoteSetKind::Global:
            return QStringLiteral("Global");
        case SeventvEmoteSetKind::Personal:
            return QStringLiteral("Personal");
        case SeventvEmoteSetKind::Channel:
            return QStringLiteral("Channel");
        default:
            return QStringLiteral("");
    }
}

Tooltip createTooltip(const QString &name, const QString &author,
                      SeventvEmoteSetKind kind)
{
    return Tooltip{QString("%1<br>%2 7TV Emote<br>By: %3")
                       .arg(name, kindToString(kind),
                            author.isEmpty() ? "<deleted>" : author)};
}

Tooltip createAliasedTooltip(const QString &name, const QString &baseName,
                             const QString &author, SeventvEmoteSetKind kind)
{
    return Tooltip{QString("%1<br>Alias of %2<br>%3 7TV Emote<br>By: %4")
                       .arg(name, baseName, kindToString(kind),
                            author.isEmpty() ? "<deleted>" : author)};
}

CreateEmoteResult createEmote(const QJsonObject &activeEmote,
                              const QJsonObject &emoteData,
                              SeventvEmoteSetKind kind)
{
    auto emoteId = EmoteId{activeEmote["id"].toString()};
    auto emoteName = EmoteName{activeEmote["name"].toString()};
    auto author =
        EmoteAuthor{emoteData["owner"].toObject()["display_name"].toString()};
    auto baseEmoteName = EmoteName{emoteData["name"].toString()};
    bool zeroWidth = isZeroWidthActive(activeEmote);
    bool aliasedName = emoteName != baseEmoteName;
    auto tooltip =
        aliasedName
            ? createAliasedTooltip(emoteName.string, baseEmoteName.string,
                                   author.string, kind)
            : createTooltip(emoteName.string, author.string, kind);
    auto imageSet = SeventvEmotes::createImageSet(emoteData);

    auto emote =
        Emote({emoteName, imageSet, tooltip,
               Url{EMOTE_LINK_FORMAT.arg(emoteId.string)}, zeroWidth, emoteId,
               author, makeConditionedOptional(aliasedName, baseEmoteName)});

    return {emote, emoteId, emoteName, !emote.images.getImage1()->isEmpty()};
}

bool checkEmoteVisibility(const QJsonObject &emoteData,
                          SeventvEmoteSetKind kind)
{
    if (!emoteData["listed"].toBool() &&
        !getSettings()->showUnlistedSevenTVEmotes)
    {
        return false;
    }

    // Only add allowed emotes
    if (kind == SeventvEmoteSetKind::Personal &&
        !emoteData["state"].toArray().contains("PERSONAL"))
    {
        return false;
    }

    auto flags =
        SeventvEmoteFlags(SeventvEmoteFlag(emoteData["flags"].toInt()));
    return !flags.has(SeventvEmoteFlag::ContentTwitchDisallowed);
}

EmotePtr createUpdatedEmote(const EmotePtr &oldEmote,
                            const EmoteUpdateDispatch &dispatch,
                            SeventvEmoteSetKind kind)
{
    bool toNonAliased = oldEmote->baseName.has_value() &&
                        dispatch.emoteName == oldEmote->baseName->string;

    auto baseName = oldEmote->baseName.value_or(oldEmote->name);
    auto emote = std::make_shared<const Emote>(Emote(
        {EmoteName{dispatch.emoteName}, oldEmote->images,
         toNonAliased
             ? createTooltip(dispatch.emoteName, oldEmote->author.string, kind)
             : createAliasedTooltip(dispatch.emoteName, baseName.string,
                                    oldEmote->author.string, kind),
         oldEmote->homePage, oldEmote->zeroWidth, oldEmote->id,
         oldEmote->author, makeConditionedOptional(!toNonAliased, baseName)}));
    return emote;
}

}  // namespace

namespace chatterino {

using namespace seventv::eventapi;
using namespace seventv::detail;
using namespace literals;

EmoteMap seventv::detail::parseEmotes(const QJsonArray &emoteSetEmotes,
                                      SeventvEmoteSetKind kind)
{
    auto emotes = EmoteMap();

    for (const auto &activeEmoteJson : emoteSetEmotes)
    {
        auto activeEmote = activeEmoteJson.toObject();
        auto emoteData = activeEmote["data"].toObject();

        if (emoteData.empty() || !checkEmoteVisibility(emoteData, kind))
        {
            continue;
        }

        auto result = createEmote(activeEmote, emoteData, kind);
        if (!result.hasImages)
        {
            // this shouldn't happen but if it does, it will crash,
            // so we don't add the emote
            qCDebug(chatterinoSeventv)
                << "Emote without images:" << activeEmote;
            continue;
        }
        auto ptr = cachedOrMake(std::move(result.emote), result.id);
        emotes[result.name] = ptr;
    }

    return emotes;
}

SeventvEmotes::SeventvEmotes()
    : global_(std::make_shared<EmoteMap>())
{
}

std::shared_ptr<const EmoteMap> SeventvEmotes::globalEmotes() const
{
    return this->global_.get();
}

std::optional<EmotePtr> SeventvEmotes::globalEmote(const EmoteName &name) const
{
    auto emotes = this->global_.get();
    auto it = emotes->find(name);

    if (it == emotes->end())
    {
        return std::nullopt;
    }
    return it->second;
}

void SeventvEmotes::loadGlobalEmotes()
{
    if (!Settings::instance().enableSevenTVGlobalEmotes)
    {
        this->setGlobalEmotes(EMPTY_EMOTE_MAP);
        return;
    }

    qCDebug(chatterinoSeventv) << "Loading 7TV Global Emotes";

    getIApp()->getSeventvAPI()->getEmoteSet(
        u"global"_s,
        [this](const auto &json) {
            QJsonArray parsedEmotes = json["emotes"].toArray();

            auto emoteMap =
                parseEmotes(parsedEmotes, SeventvEmoteSetKind::Global);
            qCDebug(chatterinoSeventv)
                << "Loaded" << emoteMap.size() << "7TV Global Emotes";
            this->setGlobalEmotes(
                std::make_shared<EmoteMap>(std::move(emoteMap)));
        },
        [](const auto &result) {
            qCWarning(chatterinoSeventv)
                << "Couldn't load 7TV global emotes" << result.getData();
        });
}

void SeventvEmotes::setGlobalEmotes(std::shared_ptr<const EmoteMap> emotes)
{
    this->global_.set(std::move(emotes));
}

void SeventvEmotes::loadChannelEmotes(
    const std::weak_ptr<Channel> &channel, const QString &channelId,
    std::function<void(EmoteMap &&, ChannelInfo)> callback, bool manualRefresh)
{
    qCDebug(chatterinoSeventv)
        << "Reloading 7TV Channel Emotes" << channelId << manualRefresh;

    getIApp()->getSeventvAPI()->getUserByTwitchID(
        channelId,
        [callback = std::move(callback), channel, channelId,
         manualRefresh](const auto &json) {
            const auto emoteSet = json["emote_set"].toObject();
            const auto parsedEmotes = emoteSet["emotes"].toArray();

            auto emoteMap =
                parseEmotes(parsedEmotes, SeventvEmoteSetKind::Channel);
            bool hasEmotes = !emoteMap.empty();

            qCDebug(chatterinoSeventv)
                << "Loaded" << emoteMap.size() << "7TV Channel Emotes for"
                << channelId << "manual refresh:" << manualRefresh;

            if (hasEmotes)
            {
                auto user = json["user"].toObject();

                size_t connectionIdx = 0;
                for (const auto &conn : user["connections"].toArray())
                {
                    if (conn.toObject()["platform"].toString() == "TWITCH")
                    {
                        break;
                    }
                    connectionIdx++;
                }

                callback(std::move(emoteMap),
                         {user["id"].toString(), emoteSet["id"].toString(),
                          connectionIdx});
            }

            auto shared = channel.lock();
            if (!shared)
            {
                return;
            }

            if (manualRefresh)
            {
                if (hasEmotes)
                {
                    shared->addMessage(
                        makeSystemMessage("7TV channel emotes reloaded."));
                }
                else
                {
                    shared->addMessage(
                        makeSystemMessage(CHANNEL_HAS_NO_EMOTES));
                }
            }
        },
        [channelId, channel, manualRefresh](const auto &result) {
            auto shared = channel.lock();
            if (!shared)
            {
                return;
            }
            if (result.status() == 404)
            {
                qCWarning(chatterinoSeventv)
                    << "Error occurred fetching 7TV emotes: "
                    << result.parseJson();
                if (manualRefresh)
                {
                    shared->addMessage(
                        makeSystemMessage(CHANNEL_HAS_NO_EMOTES));
                }
            }
            else
            {
                // TODO: Auto retry in case of a timeout, with a delay
                auto errorString = result.formatError();
                qCWarning(chatterinoSeventv)
                    << "Error fetching 7TV emotes for channel" << channelId
                    << ", error" << errorString;
                shared->addMessage(makeSystemMessage(
                    QStringLiteral("Failed to fetch 7TV channel "
                                   "emotes. (Error: %1)")
                        .arg(errorString)));
            }
        });
}

std::optional<EmotePtr> SeventvEmotes::addEmote(
    Atomic<std::shared_ptr<const EmoteMap>> &map,
    const EmoteAddDispatch &dispatch, SeventvEmoteSetKind kind)
{
    // Check for visibility first, so we don't copy the map.
    auto emoteData = dispatch.emoteJson["data"].toObject();
    if (emoteData.empty() || !checkEmoteVisibility(emoteData, kind))
    {
        return std::nullopt;
    }

    // This copies the map.
    EmoteMap updatedMap = *map.get();
    auto result = createEmote(dispatch.emoteJson, emoteData, kind);
    if (!result.hasImages)
    {
        // Incoming emote didn't contain any images, abort
        qCDebug(chatterinoSeventv)
            << "Emote without images:" << dispatch.emoteJson;
        return std::nullopt;
    }
    auto emote = std::make_shared<const Emote>(std::move(result.emote));
    updatedMap[result.name] = emote;
    map.set(std::make_shared<EmoteMap>(std::move(updatedMap)));

    return emote;
}

std::optional<EmotePtr> SeventvEmotes::updateEmote(
    Atomic<std::shared_ptr<const EmoteMap>> &map,
    const EmoteUpdateDispatch &dispatch, SeventvEmoteSetKind kind)
{
    auto oldMap = map.get();
    auto oldEmote = oldMap->findEmote(dispatch.emoteName, dispatch.emoteID);
    if (oldEmote == oldMap->end())
    {
        return std::nullopt;
    }

    // This copies the map.
    EmoteMap updatedMap = *map.get();
    updatedMap.erase(oldEmote->second->name);

    auto emote = createUpdatedEmote(oldEmote->second, dispatch, kind);
    updatedMap[emote->name] = emote;
    map.set(std::make_shared<EmoteMap>(std::move(updatedMap)));

    return emote;
}

std::optional<EmotePtr> SeventvEmotes::removeEmote(
    Atomic<std::shared_ptr<const EmoteMap>> &map,
    const EmoteRemoveDispatch &dispatch)
{
    // This copies the map.
    EmoteMap updatedMap = *map.get();
    auto it = updatedMap.findEmote(dispatch.emoteName, dispatch.emoteID);
    if (it == updatedMap.end())
    {
        // We already copied the map at this point and are now discarding the copy.
        // This is fine, because this case should be really rare.
        return std::nullopt;
    }
    auto emote = it->second;
    updatedMap.erase(it);
    map.set(std::make_shared<EmoteMap>(std::move(updatedMap)));

    return emote;
}

void SeventvEmotes::getEmoteSet(
    const QString &emoteSetId,
    std::function<void(EmoteMap &&, QString)> successCallback,
    std::function<void(QString)> errorCallback)
{
    qCDebug(chatterinoSeventv) << "Loading 7TV Emote Set" << emoteSetId;

    getIApp()->getSeventvAPI()->getEmoteSet(
        emoteSetId,
        [callback = std::move(successCallback), emoteSetId](const auto &json) {
            auto parsedEmotes = json["emotes"].toArray();

            auto kind = SeventvEmoteSetKind::Channel;
            if (SeventvEmoteSetFlags(SeventvEmoteSetFlag(json["flags"].toInt()))
                    .has(SeventvEmoteSetFlag::Personal))
            {
                kind = SeventvEmoteSetKind::Personal;
            }

            auto emoteMap = parseEmotes(parsedEmotes, kind);

            qCDebug(chatterinoSeventv) << "Loaded" << emoteMap.size()
                                       << "7TV Emotes from" << emoteSetId;

            callback(std::move(emoteMap), json["name"].toString());
        },
        [emoteSetId, callback = std::move(errorCallback)](const auto &result) {
            callback(result.formatError());
        });
}

ImageSet SeventvEmotes::createImageSet(const QJsonObject &emoteData)
{
    const auto host = emoteData["host"].toObject();
    // "//cdn.7tv[...]"
    auto baseUrl = host["url"].toString();
    const auto files = host["files"].toArray();

    std::array<ImagePtr, 4> sizes;
    double baseWidth = 0.0;
    size_t nextSize = 0;

    auto targetFormat = [&] {
        if (!ALLOW_AVIF_IMAGES() || files.empty())
        {
            return u"WEBP"_s;
        }
        // Look at the first two images and guess the target format.
        auto first = files[0]["format"_L1].toString();
        if (files.size() < 2)
        {
            return first;
        }
        auto second = files[1]["format"_L1].toString();
        if (first == u"AVIF"_s || second == u"AVIF"_s)
        {
            // prefer avif
            return u"AVIF"_s;
        }
        // fallback
        return u"WEBP"_s;
    }();

    for (auto fileItem : files)
    {
        if (nextSize >= sizes.size())
        {
            break;
        }

        auto file = fileItem.toObject();
        if (file["format"].toString() != targetFormat)
        {
            continue;  // TODO: support fallbacks
        }

        double width = file["width"].toDouble();
        double scale = 1.0;  // in relation to first image
        if (baseWidth > 0.0)
        {
            scale = baseWidth / width;
        }
        else
        {
            // => this is the first image
            baseWidth = width;
        }

        auto image = Image::fromUrl(
            {QString("https:%1/%2").arg(baseUrl, file["name"].toString())},
            scale, {static_cast<int>(width), file["height"].toInt(16)});

        sizes.at(nextSize) = image;
        nextSize++;
    }

    if (nextSize < sizes.size())
    {
        // this should be really rare
        // this means we didn't get all sizes of an emote
        if (nextSize == 0)
        {
            qCDebug(chatterinoSeventv)
                << "Got file list without any eligible files";
            // When this emote is typed, chatterino will crash.
            return ImageSet{};
        }
        for (; nextSize < sizes.size(); nextSize++)
        {
            sizes.at(nextSize) = Image::getEmpty();
        }
    }

    // Typically, 7TV provides four versions (1x, 2x, 3x, and 4x). The 3x
    // version has a scale factor of 1/3, which is a size other providers don't
    // provide - they only provide the 4x version (0.25). To be in line with
    // other providers, we prefer the 4x version but fall back to the 3x one if
    // it doesn't exist.
    auto largest = std::move(sizes[3]);
    if (!largest || largest->isEmpty())
    {
        largest = std::move(sizes[2]);
    }

    return ImageSet{sizes[0], sizes[1], largest};
}

}  // namespace chatterino
