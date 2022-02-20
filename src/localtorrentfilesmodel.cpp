/*
 * Tremotesf
 * Copyright (C) 2015-2018 Alexey Rochev <equeim@gmail.com>
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
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "localtorrentfilesmodel.h"

#include <QApplication>
#include <QCoreApplication>
#include <QFutureWatcher>
#include <QStyle>
#include <QtConcurrentRun>

#include "bencodeparser.h"
#include "torrentfilesmodelentry.h"
#include "utils.h"

namespace tremotesf
{
    namespace
    {
        using namespace std::string_view_literals;

        constexpr auto infoKey = "info"sv;
        constexpr auto filesKey = "files"sv;
        constexpr auto nameKey = "name"sv;
        constexpr auto pathKey = "path"sv;
        constexpr auto lengthKey = "length"sv;

        template<typename T, bool WarnOnNoKey = true>
        std::optional<T> findValue(bencode::Dictionary& dict, std::string_view key, std::string_view dictName, const char* valueType)
        {
            if (auto found = dict.find(key); found != dict.end()) {
                auto maybeValue = found->second.maybeTakeValue<T>();
                if (!maybeValue) {
                    qWarning("createTree: %s dictionary value for key %s is not of %s type", dictName.data(), key.data(), valueType);
                }
                return maybeValue;
            }
            if constexpr (WarnOnNoKey) {
                qWarning("createTree: %s dictionary doesn't have key %s", dictName.data(), key.data());
            }
            return std::nullopt;
        }

        struct CreateTreeResult
        {
            std::shared_ptr<TorrentFilesModelDirectory> rootDirectory;
            std::vector<TorrentFilesModelFile*> files;
        };

        std::optional<CreateTreeResult> createTree(bencode::Value&& parseResult)
        {
            auto rootMap = parseResult.maybeTakeDictionary();
            if (!rootMap) {
                qWarning("createTree: root element is not a dictionary");
                return {};
            }

            auto infoMap = findValue<bencode::Dictionary>(*rootMap, infoKey, "root", "dictionary");
            if (!infoMap) {
                return {};
            }

            auto rootDirectory = std::make_shared<TorrentFilesModelDirectory>();
            std::vector<TorrentFilesModelFile*> files;

            if (auto filesList = findValue<bencode::List, false>(*infoMap, filesKey, infoKey, "list"); filesList) {
                const auto torrentDirectoryName = findValue<QString>(*infoMap, nameKey, infoKey, "string");
                if (!torrentDirectoryName) {
                    return {};
                }

                auto* torrentDirectory = rootDirectory->addDirectory(*torrentDirectoryName);

                const auto filesCount = filesList->size();
                files.reserve(filesCount);
                int fileIndex = -1;
                for (auto& fileValue : *filesList) {
                    ++fileIndex;

                    auto fileMap = fileValue.maybeTakeDictionary();
                    if (!fileMap) {
                        qWarning("createTree: files list element at index %d is not a dictionary", fileIndex);
                        return {};
                    }

                    TorrentFilesModelDirectory* currentDirectory = torrentDirectory;

                    auto pathParts = findValue<bencode::List>(*fileMap, pathKey, filesKey, "list");
                    if (!pathParts) {
                        return {};
                    }

                    int partIndex = -1;
                    const int lastPartIndex = static_cast<int>(pathParts->size()) - 1;

                    for (auto& partValue : *pathParts) {
                        ++partIndex;

                        auto part = partValue.maybeTakeString();
                        if (!part) {
                            qWarning("createTree: path element '%d' for file at index '%d' is not a string", partIndex, fileIndex);
                            return {};
                        }

                        if (partIndex == lastPartIndex) {
                            auto length = findValue<bencode::Integer>(*fileMap, lengthKey, filesKey, "integer");
                            if (!length) {
                                return {};
                            }

                            auto* childFile = currentDirectory->addFile(fileIndex, *part, *length);
                            childFile->setWanted(true);
                            childFile->setPriority(TorrentFilesModelEntry::NormalPriority);
                            childFile->setChanged(false);
                            files.push_back(childFile);
                        } else {
                            const auto& childrenHash = currentDirectory->childrenHash();
                            const auto found = childrenHash.find(*part);
                            if (found != childrenHash.end()) {
                                currentDirectory = static_cast<TorrentFilesModelDirectory*>(found->second);
                            } else {
                                currentDirectory = currentDirectory->addDirectory(*part);
                            }
                        }
                    }
                }
            } else {
                auto name = findValue<QString>(*infoMap, nameKey, infoKey, "string");
                if (!name) {
                    return {};
                }
                auto length = findValue<bencode::Integer>(*infoMap, lengthKey, infoKey, "integer");
                if (!length) {
                    return {};
                }

                auto* file = rootDirectory->addFile(0, *name, *length);
                file->setWanted(true);
                file->setPriority(TorrentFilesModelEntry::NormalPriority);
                file->setChanged(false);
                files.push_back(file);
            }

            return CreateTreeResult{std::move(rootDirectory), std::move(files)};
        }
    }

    LocalTorrentFilesModel::LocalTorrentFilesModel(QObject* parent)
        : BaseTorrentFilesModel({NameColumn, SizeColumn, PriorityColumn}, parent),
          mLoaded(false)
    {

    }

    void LocalTorrentFilesModel::load(const QString& filePath)
    {
        beginResetModel();

        using FutureResult = std::variant<CreateTreeResult, bencode::Error::Type>;
        using FutureWatcher = QFutureWatcher<FutureResult>;

        const auto future = QtConcurrent::run([=]() -> FutureResult {
            try {
                auto parseResult = bencode::parse(filePath);
                if (auto createTreeResult = createTree(std::move(parseResult)); createTreeResult) {
                    return std::move(*createTreeResult);
                }
                qWarning() << "Failed to parse torrent file" << filePath;
                return bencode::Error::Type::Parsing;
            } catch (const bencode::Error& e) {
                qWarning() << "Failed to parse torrent file" << filePath << ":" << e.what();
                return e.type();
            }
        });

        auto watcher = new FutureWatcher(this);
        QObject::connect(watcher, &FutureWatcher::finished, this, [=] {
            auto result = watcher->result();
            if (auto createTreeResult = std::get_if<CreateTreeResult>(&result); createTreeResult) {
                mRootDirectory = std::move(createTreeResult->rootDirectory);
                mFiles = std::move(createTreeResult->files);
            } else {
                mErrorType = std::get<bencode::Error::Type>(result);
            }
            endResetModel();
            mLoaded = true;
            emit loadedChanged();
            watcher->deleteLater();
        });
        watcher->setFuture(future);
    }

    bool LocalTorrentFilesModel::isLoaded() const
    {
        return mLoaded;
    }

    bool LocalTorrentFilesModel::isSuccessfull() const
    {
        return !mErrorType.has_value();
    }

    QString LocalTorrentFilesModel::errorString() const
    {
        if (!mErrorType) {
            return {};
        }
        switch (*mErrorType) {
        case bencode::Error::Type::Reading:
            return qApp->translate("tremotesf", "Error reading torrent file");
        case bencode::Error::Type::Parsing:
            return qApp->translate("tremotesf", "Error parsing torrent file");
        default:
            return {};
        }
    }

    QVariantList LocalTorrentFilesModel::unwantedFiles() const
    {
        QVariantList files;
        for (const TorrentFilesModelFile* file : mFiles) {
            if (file->wantedState() == TorrentFilesModelEntry::Unwanted) {
                files.append(file->id());
            }
        }
        return files;
    }

    QVariantList LocalTorrentFilesModel::highPriorityFiles() const
    {
        QVariantList files;
        for (const TorrentFilesModelFile* file : mFiles) {
            if (file->priority() == TorrentFilesModelEntry::HighPriority) {
                files.append(file->id());
            }
        }
        return files;
    }

    QVariantList LocalTorrentFilesModel::lowPriorityFiles() const
    {
        QVariantList files;
        for (const TorrentFilesModelFile* file : mFiles) {
            if (file->priority() == TorrentFilesModelEntry::LowPriority) {
                files.append(file->id());
            }
        }
        return files;
    }

    const QVariantMap& LocalTorrentFilesModel::renamedFiles() const
    {
        return mRenamedFiles;
    }

    void LocalTorrentFilesModel::setFileWanted(const QModelIndex& index, bool wanted)
    {
        BaseTorrentFilesModel::setFileWanted(index, wanted);
        emit wantedFilesChanged();
    }

    void LocalTorrentFilesModel::setFilesWanted(const QModelIndexList& indexes, bool wanted)
    {
        BaseTorrentFilesModel::setFilesWanted(indexes, wanted);
        emit wantedFilesChanged();
    }

    void LocalTorrentFilesModel::setFilePriority(const QModelIndex& index, TorrentFilesModelEntry::Priority priority)
    {
        BaseTorrentFilesModel::setFilePriority(index, priority);
        emit filesPriorityChanged();
    }

    void LocalTorrentFilesModel::setFilesPriority(const QModelIndexList& indexes, TorrentFilesModelEntry::Priority priority)
    {
        BaseTorrentFilesModel::setFilesPriority(indexes, priority);
        emit filesPriorityChanged();
    }

    void LocalTorrentFilesModel::renameFile(const QModelIndex& index, const QString& newName)
    {
        auto entry = static_cast<TorrentFilesModelEntry*>(index.internalPointer());
        fileRenamed(entry, newName);
        mRenamedFiles.insert(entry->path(), newName);
        emit renamedFilesChanged();
    }
}
