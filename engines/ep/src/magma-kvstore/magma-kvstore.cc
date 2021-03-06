/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
/*
 *     Copyright 2018 Couchbase, Inc.
 *
 *   Licensed under the Apache License, Version 2.0 (the "License");
 *   you may not use this file except in compliance with the License.
 *   You may obtain a copy of the License at
 *
 *       http://www.apache.org/licenses/LICENSE-2.0
 *
 *   Unless required by applicable law or agreed to in writing, software
 *   distributed under the License is distributed on an "AS IS" BASIS,
 *   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *   See the License for the specific language governing permissions and
 *   limitations under the License.
 */

#include "magma-kvstore.h"
#include "bucket_logger.h"
#include "ep_time.h"
#include "item.h"
#include "magma-kvstore_config.h"
#include "statwriter.h"
#include "vb_commit.h"
#include "vbucket.h"
#include "vbucket_state.h"

#include <mcbp/protocol/request.h>
#include <nlohmann/json.hpp>
#include <utilities/logtags.h>

#include <string.h>
#include <algorithm>
#include <limits>

class Snapshot;

// Unfortunately, turning on logging for the tests is limited to debug
// mode. While we are in the midst of dropping in magma, this provides
// a simple way to change all the logging->trace calls to logging->debug
// by changing trace to debug..
// Once magma has been completed, we can remove this #define and change
// all the logging calls back to trace.
#define TRACE trace

using namespace magma;
using namespace std::chrono_literals;

namespace magmakv {
// MetaData is used to serialize and de-serialize metadata respectively when
// writing a Document mutation request to Magma and when reading a Document
// from Magma.

// The `#pragma pack(1)` directive and the order of members are to keep
// the size of MetaData as small as possible and uniform across different
// platforms.
#pragma pack(1)
class MetaData {
public:
    // The Operation this represents - maps to queue_op types:
    enum class Operation {
        // A standard mutation (or deletion). Present in the 'normal'
        // (committed) namespace.
        Mutation,

        // A prepared SyncWrite. `durability_level` field indicates the level
        // Present in the DurabilityPrepare namespace.
        PreparedSyncWrite,

        // A committed SyncWrite.
        // This exists so we can correctly backfill from disk a Committed
        // mutation and sent out as a DCP_COMMIT to sync_replication
        // enabled DCP clients.
        // Present in the 'normal' (committed) namespace.
        CommittedSyncWrite,

        // An aborted SyncWrite.
        // This exists so we can correctly backfill from disk an Aborted
        // mutation and sent out as a DCP_ABORT to sync_replication
        // enabled DCP clients.
        // Present in the DurabilityPrepare namespace.
        Abort,
    };

    MetaData() = default;

    MetaData(const Item& it)
        : metaDataVersion(0),
          bySeqno(it.getBySeqno()),
          cas(it.getCas()),
          revSeqno(it.getRevSeqno()),
          exptime(it.getExptime()),
          flags(it.getFlags()),
          valueSize(it.getNBytes()),
          vbid(it.getVBucketId().get()),
          datatype(it.getDataType()),
          prepareSeqno(it.getPrepareSeqno()) {
        if (it.isDeleted()) {
            deleted = 1;
            deleteSource = static_cast<uint8_t>(it.deletionSource());
        } else {
            deleted = 0;
            deleteSource = 0;
        }
        operation = static_cast<uint8_t>(toOperation(it.getOperation()));
        durabilityLevel =
                static_cast<uint8_t>(it.getDurabilityReqs().getLevel());
    };

    // Magma requires meta data for local documents. Rather than support 2
    // different meta data versions, we simplify by using just 1.
    MetaData(bool isDeleted, uint32_t valueSize, int64_t seqno, Vbid vbid)
        : metaDataVersion(0),
          bySeqno(seqno),
          cas(0),
          revSeqno(0),
          exptime(0),
          flags(0),
          valueSize(valueSize),
          vbid(vbid.get()),
          datatype(0),
          operation(0),
          durabilityLevel(0),
          prepareSeqno(0) {
        if (isDeleted) {
            deleted = 1;
            deleteSource = static_cast<uint8_t>(DeleteSource::Explicit);
        } else {
            deleted = 0;
            deleteSource = 0;
        }
    };

    Operation getOperation() const {
        return static_cast<Operation>(operation);
    }

    cb::durability::Level getDurabilityLevel() const {
        return static_cast<cb::durability::Level>(durabilityLevel);
    }

    std::string to_string(Operation op) const {
        switch (op) {
        case Operation::Mutation:
            return "Mutation";
        case Operation::PreparedSyncWrite:
            return "PreparedSyncWrite";
        case Operation::CommittedSyncWrite:
            return "CommittedSyncWrite";
        case Operation::Abort:
            return "Abort";
        }
        return std::string{"Unknown:" +
                           std::to_string(static_cast<uint8_t>(op))};
    }

    std::string to_string() const {
        std::stringstream ss;
        int vers = metaDataVersion;
        int dt = datatype;
        cb::durability::Requirements req(getDurabilityLevel(), {});
        ss << "bySeqno:" << bySeqno << " cas:" << cas << " exptime:" << exptime
           << " revSeqno:" << revSeqno << " flags:" << flags
           << " valueSize:" << valueSize << " vbid:" << vbid
           << " deleted:" << (deleted == 0 ? "false" : "true")
           << " deleteSource:"
           << (deleted == 0 ? " " : deleteSource == 0 ? "Explicit" : "TTL")
           << " version:" << vers << " datatype:" << dt
           << " operation:" << to_string(getOperation())
           << " durabilityLevel:" << cb::durability::to_string(req);
        return ss.str();
    }

    uint8_t metaDataVersion;
    int64_t bySeqno;
    uint64_t cas;
    uint64_t revSeqno;
    uint32_t exptime;
    uint32_t flags;
    uint32_t valueSize;
    uint16_t vbid;
    uint8_t datatype;
    uint8_t deleted : 1;
    uint8_t deleteSource : 1;
    uint8_t operation : 2;
    uint8_t durabilityLevel : 2;
    cb::uint48_t prepareSeqno;

private:
    static Operation toOperation(queue_op op) {
        switch (op) {
        case queue_op::mutation:
        case queue_op::system_event:
            return Operation::Mutation;
        case queue_op::pending_sync_write:
            return Operation::PreparedSyncWrite;
        case queue_op::commit_sync_write:
            return Operation::CommittedSyncWrite;
        case queue_op::abort_sync_write:
            return Operation::Abort;
        case queue_op::flush:
        case queue_op::empty:
        case queue_op::checkpoint_start:
        case queue_op::checkpoint_end:
        case queue_op::set_vbucket_state:
            break;
        }
        throw std::invalid_argument(
                "magma::MetaData::toOperation: Unsupported op " +
                std::to_string(static_cast<uint8_t>(op)));
    }
};
#pragma pack()

static_assert(sizeof(MetaData) == 47,
              "magmakv::MetaData is not the expected size.");
} // namespace magmakv

// Keys to localdb docs
static const Slice vbstateKey = {"_vbstate"};
static const Slice manifestKey = {"_collections/manifest"};
static const Slice openCollectionsKey = {"_collections/open"};
static const Slice openScopesKey = {"_scopes/open"};
static const Slice droppedCollectionsKey = {"_collections/dropped"};

/**
 * Class representing a document to be persisted in Magma.
 */
class MagmaRequest : public IORequest {
public:
    /**
     * Constructor
     *
     * @param item Item instance to be persisted
     * @param callback Persistence Callback
     * @param logger Used for logging
     */
    MagmaRequest(queued_item it, std::shared_ptr<BucketLogger> logger)
        : IORequest(std::move(it)),
          docMeta(magmakv::MetaData(*item)),
          docBody(item->getValue()) {
        if (logger->should_log(spdlog::level::TRACE)) {
            logger->TRACE("MagmaRequest:{}", to_string());
        }
    }

    magmakv::MetaData& getDocMeta() {
        return docMeta;
    }

    Vbid getVbID() const {
        return Vbid(docMeta.vbid);
    }

    size_t getKeyLen() const {
        return key.size();
    }

    const char* getKey() const {
        return reinterpret_cast<const char*>(key.data());
    }

    size_t getBodySize() const {
        return docBody ? docBody->valueSize() : 0;
    }

    char* getBodyData() const {
        return docBody ? const_cast<char*>(docBody->getData()) : nullptr;
    }

    size_t getMetaSize() const {
        return sizeof(magmakv::MetaData);
    }

    void markOldItemExists() {
        itemOldExists = true;
    }

    bool oldItemExists() const {
        return itemOldExists;
    }

    void markOldItemIsDelete() {
        itemOldIsDelete = true;
    }

    bool oldItemIsDelete() const {
        return itemOldIsDelete;
    }

    void markRequestFailed() {
        reqFailed = true;
    }

    bool requestFailed() const {
        return reqFailed;
    }

    std::string to_string() {
        std::stringstream ss;
        ss << "Key:" << key.to_string() << " docMeta:" << docMeta.to_string()
           << " itemOldExists:" << (itemOldExists ? "true" : "false")
           << " itemOldIsDelete:" << (itemOldIsDelete ? "true" : "false")
           << " reqFailed:" << (reqFailed ? "true" : "false");
        return ss.str();
    }

private:
    magmakv::MetaData docMeta;
    value_t docBody;
    bool itemOldExists{false};
    bool itemOldIsDelete{false};
    bool reqFailed{false};
};

/**
 * Magma stores an item in 3 slices... key, metadata, value
 * See libmagma/slice.h for more info on slices.
 *
 * Helper functions to pull metadata stuff out of the metadata slice.
 * The are used down in magma and are passed in as part of the configuration.
 */
static const uint64_t getSeqNum(const Slice& metaSlice) {
    magmakv::MetaData* docMeta = reinterpret_cast<magmakv::MetaData*>(
            const_cast<char*>(metaSlice.Data()));
    return docMeta->bySeqno;
}

static const size_t getValueSize(const Slice& metaSlice) {
    const auto* docMeta =
            reinterpret_cast<const magmakv::MetaData*>(metaSlice.Data());
    return docMeta->valueSize;
}

static const uint32_t getExpiryTime(const Slice& metaSlice) {
    magmakv::MetaData* docMeta = reinterpret_cast<magmakv::MetaData*>(
            const_cast<char*>(metaSlice.Data()));
    return docMeta->exptime;
}

static const bool isDeleted(const Slice& metaSlice) {
    magmakv::MetaData* docMeta = reinterpret_cast<magmakv::MetaData*>(
            const_cast<char*>(metaSlice.Data()));
    return docMeta->deleted > 0 ? true : false;
}

static const bool isCompressed(const Slice& metaSlice) {
    magmakv::MetaData* docMeta = reinterpret_cast<magmakv::MetaData*>(
            const_cast<char*>(metaSlice.Data()));
    return mcbp::datatype::is_snappy(docMeta->datatype);
}

static const Vbid getVbid(const Slice& metaSlice) {
    magmakv::MetaData* docMeta = reinterpret_cast<magmakv::MetaData*>(
            const_cast<char*>(metaSlice.Data()));
    return Vbid(docMeta->vbid);
}

static const magmakv::MetaData& getDocMeta(const Slice& metaSlice) {
    magmakv::MetaData* docMeta = reinterpret_cast<magmakv::MetaData*>(
            const_cast<char*>(metaSlice.Data()));
    return *docMeta;
}

static const bool isPrepared(const Slice& metaSlice) {
    magmakv::MetaData* docMeta = reinterpret_cast<magmakv::MetaData*>(
            const_cast<char*>(metaSlice.Data()));
    return static_cast<magmakv::MetaData::Operation>(docMeta->operation) ==
           magmakv::MetaData::Operation::PreparedSyncWrite;
}

static DiskDocKey makeDiskDocKey(const Slice& key) {
    return DiskDocKey{key.Data(), key.Len()};
}

MagmaKVStore::MagmaCompactionCB::MagmaCompactionCB(MagmaKVStore& magmaKVStore)
    : magmaKVStore(magmaKVStore) {
    magmaKVStore.logger->TRACE("MagmaCompactionCB constructor");
    initialized = false;
}

MagmaKVStore::MagmaCompactionCB::~MagmaCompactionCB() {
    magmaKVStore.logger->debug("MagmaCompactionCB destructor");
}

bool MagmaKVStore::compactionCallBack(MagmaKVStore::MagmaCompactionCB& cbCtx,
                                      const magma::Slice& keySlice,
                                      const magma::Slice& metaSlice,
                                      const magma::Slice& valueSlice) {
    auto& itemString = cbCtx.itemKeyBuf;
    if (logger->should_log(spdlog::level::TRACE)) {
        itemString.clear();
        itemString << "key:"
                   << cb::UserData{makeDiskDocKey(keySlice).to_string()};
        itemString << " ";
        itemString << getDocMeta(metaSlice).to_string();
        logger->TRACE("MagmaCompactionCB {}", itemString.str());
    }

    if (!cbCtx.initialized) {
        cbCtx.vbid = getVbid(metaSlice);

        // If we have a compaction_ctx, that means we are doing an
        // explicit compaction triggered by the kv_engine.
        {
            std::lock_guard<std::mutex> lock(compactionCtxMutex);
            if (compaction_ctxList[cbCtx.vbid.get()]) {
                cbCtx.magmaCompactionCtx = compaction_ctxList[cbCtx.vbid.get()];
                cbCtx.ctx = cbCtx.magmaCompactionCtx->ctx;
            }
        }
        if (!cbCtx.ctx) {
            // TODO: we don't have a callback function to kv_engine
            // to grab the compaction_ctx when the compaction is
            // triggered by magma (Level, Expiry).
        }
        cbCtx.initialized = true;
    }

    if (cbCtx.ctx) {
        auto seqno = getSeqNum(metaSlice);
        auto exptime = getExpiryTime(metaSlice);
        Status status;

        if (cbCtx.ctx->droppedKeyCb) {
            // We need to check both committed and prepared documents - if the
            // collection has been logically deleted then we need to discard
            // both types of keys.
            // As such use the docKey (i.e. without any DurabilityPrepare
            // namespace) when checking if logically deleted;
            auto diskKey = makeDiskDocKey(keySlice);
            if (cbCtx.ctx->eraserContext->isLogicallyDeleted(
                        diskKey.getDocKey(), seqno)) {
                // Inform vb that the key@seqno is dropped
                cbCtx.ctx->droppedKeyCb(diskKey, seqno);
                if (!isDeleted(metaSlice)) {
                    cbCtx.ctx->stats.collectionsItemsPurged++;
                } else {
                    if (isPrepared(metaSlice)) {
                        cbCtx.ctx->stats.preparesPurged++;
                    }
                    cbCtx.ctx->stats.collectionsDeletedItemsPurged++;
                }
                return true;
            }
        }

        if (isDeleted(metaSlice)) {
            uint64_t maxSeqno;
            status = magma->GetMaxSeqno(cbCtx.vbid.get(), maxSeqno);
            if (!status) {
                throw std::runtime_error("MagmaCompactionCallback Failed : " +
                                         status.String());
            }

            if (seqno != maxSeqno) {
                if (cbCtx.ctx->compactConfig.drop_deletes) {
                    logger->TRACE("MagmaCompactionCB DROP drop_deletes {}",
                                  itemString.str());
                    cbCtx.ctx->stats.tombstonesPurged++;
                    if (cbCtx.ctx->max_purged_seq < seqno) {
                        cbCtx.ctx->max_purged_seq = seqno;
                    }
                    return true;
                }

                if (exptime < cbCtx.ctx->compactConfig.purge_before_ts &&
                    (exptime ||
                     !cbCtx.ctx->compactConfig.retain_erroneous_tombstones) &&
                    (!cbCtx.ctx->compactConfig.purge_before_seq ||
                     seqno <= cbCtx.ctx->compactConfig.purge_before_seq)) {
                    logger->TRACE("MagmaCompactionCB DROP expired tombstone {}",
                                  itemString.str());
                    cbCtx.ctx->stats.tombstonesPurged++;
                    if (cbCtx.ctx->max_purged_seq < seqno) {
                        cbCtx.ctx->max_purged_seq = seqno;
                    }
                    return true;
                }
            }
        } else {
            // We can remove any prepares that have been completed. This works
            // because we send Mutations instead of Commits when streaming from
            // Disk so we do not need to send a Prepare message to keep things
            // consistent on a replica.
            if (isPrepared(metaSlice)) {
                if (cbCtx.ctx->max_purged_seq < seqno) {
                    cbCtx.ctx->stats.preparesPurged++;
                    return true;
                }
            }
            time_t currTime = ep_real_time();
            if (exptime && exptime < currTime) {
                auto docMeta = getDocMeta(metaSlice);
                auto itm = std::make_unique<Item>(
                        makeDiskDocKey(keySlice).getDocKey(),
                        docMeta.flags,
                        docMeta.exptime,
                        valueSlice.Data(),
                        valueSlice.Len(),
                        docMeta.datatype,
                        docMeta.cas,
                        docMeta.bySeqno,
                        cbCtx.vbid,
                        docMeta.revSeqno);
                if (isCompressed(metaSlice)) {
                    itm->decompressValue();
                }
                itm->setDeleted(DeleteSource::TTL);
                cbCtx.ctx->expiryCallback->callback(*(itm.get()), currTime);
                logger->TRACE("MagmaCompactionCB expiry callback {}",
                              itemString.str());
            }
        }
    }

    logger->TRACE("MagmaCompactionCB KEEP {}", itemString.str());
    return false;
}

MagmaKVStore::MagmaKVStore(MagmaKVStoreConfig& configuration)
    : KVStore(),
      configuration(configuration),
      pendingReqs(std::make_unique<PendingRequestQueue>()),
      in_transaction(false),
      magmaPath(configuration.getDBName() + "/magma." +
                std::to_string(configuration.getShardId())),
      scanCounter(0),
      cachedMagmaInfo(configuration.getMaxVBuckets()),
      compaction_ctxList(configuration.getMaxVBuckets()) {
    configuration.magmaCfg.Path = magmaPath;
    configuration.magmaCfg.MaxKVStores = configuration.getMaxVBuckets();
    configuration.magmaCfg.MaxKVStoreLSDBufferSize =
            configuration.getMagmaDeleteMemtableWritecache();
    configuration.magmaCfg.LSDFragmentationRatio =
            configuration.getMagmaDeleteFragRatio();
    configuration.magmaCfg.MaxCommitPoints =
            configuration.getMagmaMaxCommitPoints();
    auto commitPointInterval = configuration.getMagmaCommitPointInterval();
    configuration.magmaCfg.CommitPointInterval =
            commitPointInterval * std::chrono::milliseconds{1s};
    configuration.magmaCfg.MinValueSize =
            configuration.getMagmaValueSeparationSize();
    configuration.magmaCfg.MaxWriteCacheSize =
            configuration.getMagmaMaxWriteCache();
    configuration.magmaCfg.WALBufferSize =
            configuration.getMagmaWalBufferSize();
    configuration.magmaCfg.NumWALBuffers =
            configuration.getMagmaWalNumBuffers();
    configuration.magmaCfg.NumFlushers = configuration.getMagmaNumFlushers();
    configuration.magmaCfg.NumCompactors =
            configuration.getMagmaNumCompactors();
    configuration.magmaCfg.WALSyncTime = 0ms;
    configuration.magmaCfg.ExpiryFragThreshold =
            configuration.getMagmaExpiryFragThreshold();
    configuration.magmaCfg.TombstoneFragThreshold =
            configuration.getMagmaTombstoneFragThreshold();
    configuration.magmaCfg.GetSeqNum = getSeqNum;
    configuration.magmaCfg.GetExpiryTime = getExpiryTime;
    configuration.magmaCfg.GetValueSize = getValueSize;
    configuration.magmaCfg.IsTombstone = isDeleted;
    configuration.magmaCfg.EnableDirectIO =
            configuration.getMagmaEnableDirectIo();
    configuration.magmaCfg.EnableBlockCache =
            configuration.getMagmaEnableBlockCache();
    configuration.magmaCfg.WriteCacheRatio =
            configuration.getMagmaWriteCacheRatio();

    magma::SetMaxOpenFiles(configuration.getMaxFileDescriptors());

    cachedVBStates.resize(configuration.getMaxVBuckets());

    commitPointEveryBatch = configuration.getMagmaCommitPointEveryBatch();
    useUpsertForSet = configuration.getMagmaEnableUpsert();
    if (useUpsertForSet) {
        configuration.magmaCfg.EnableUpdateStatusForSet = false;
    } else {
        configuration.magmaCfg.EnableUpdateStatusForSet = true;
    }

    // Set up thread and memory tracking.
    auto currEngine = ObjectRegistry::getCurrentEngine();
    configuration.magmaCfg.SetupThreadContext = [currEngine]() {
        ObjectRegistry::onSwitchThread(currEngine, false);
    };
    configuration.magmaCfg.ResetThreadContext = []() {
        ObjectRegistry::onSwitchThread(nullptr);
    };

    configuration.magmaCfg.MakeCompactionCallback = [&]() {
        return std::make_unique<MagmaKVStore::MagmaCompactionCB>(*this);
    };

    // Create the data directory.
    createDataDir(configuration.magmaCfg.Path);

    // Create a logger that is prefixed with magma so that all code from
    // wrapper down through magma use this logger.
    std::string loggerName =
            "magma_" + std::to_string(configuration.getShardId());
    logger = BucketLogger::createBucketLogger(loggerName, loggerName);
    configuration.magmaCfg.LogContext = logger;

    // Open the magma instance for this shard and populate the
    // vbstate and magmaInfo.
    magma = std::make_unique<Magma>(configuration.magmaCfg);
    auto status = magma->Open();
    if (!status) {
        std::string err =
                "MagmaKVStore Magma open failed. Status:" + status.String();
        logger->critical(err);
        throw std::logic_error(err);
    }

    setMaxDataSize(configuration.getBucketQuota());

    auto kvstoreList = magma->GetKVStoreList();
    for (auto& kvid : kvstoreList) {
        status = loadVBStateCache(Vbid(kvid));
        if (!status) {
            throw std::logic_error("MagmaKVStore vbstate vbid:" +
                                   std::to_string(kvid) +
                                   " not found."
                                   " Status:" +
                                   status.String());
        }
    }
}

MagmaKVStore::~MagmaKVStore() {
    if (!in_transaction) {
        magma->Sync(true);
    }
    logger->debug("MagmaKVStore Destructor");
}

std::string MagmaKVStore::getVBDBSubdir(Vbid vbid) {
    return magmaPath + std::to_string(vbid.get());
}

bool MagmaKVStore::begin(std::unique_ptr<TransactionContext> txCtx) {
    in_transaction = true;
    transactionCtx = std::move(txCtx);
    return in_transaction;
}

bool MagmaKVStore::commit(VB::Commit& commitData) {
    // This behaviour is to replicate the one in Couchstore.
    // If `commit` is called when not in transaction, just return true.
    if (!in_transaction) {
        logger->warn("MagmaKVStore::commit called not in transaction");
        return true;
    }

    if (pendingReqs->size() == 0) {
        in_transaction = false;
        return true;
    }

    kvstats_ctx kvctx(commitData);
    bool success = true;

    // Flush all documents to disk
    auto errCode = saveDocs(commitData, kvctx);
    if (errCode != ENGINE_SUCCESS) {
        logger->warn("MagmaKVStore::commit: saveDocs {} errCode:{}",
                     pendingReqs->front().getVbID(),
                     errCode);
        success = false;
    }

    commitCallback(errCode, kvctx);

    // This behaviour is to replicate the one in Couchstore.
    // Set `in_transanction = false` only if `commit` is successful.
    if (success) {
        in_transaction = false;
        transactionCtx.reset();
    }

    pendingReqs->clear();
    logger->TRACE("MagmaKVStore::commit success:{}", success);

    return success;
}

void MagmaKVStore::commitCallback(int errCode, kvstats_ctx&) {
    for (const auto& req : *pendingReqs) {
        if (errCode) {
            ++st.numSetFailure;
        }

        size_t mutationSize =
                req.getKeyLen() + req.getBodySize() + req.getMetaSize();
        st.io_num_write++;
        st.io_document_write_bytes += mutationSize;

        // Note:
        // There are 3 cases for setting rv based on errCode
        // MutationStatus::MutationSuccess
        // MutationStatus::DocNotFound
        // MutationStatus::MutationFailed - magma does not support this case
        // because
        //     we don't want to requeue the item.  If errCode is bad,
        //     its most likely something fatal with the kvstore
        //     or it might be the kvstore is pending being dropped.
        auto rv = MutationStatus::DocNotFound;
        if (req.oldItemExists() && !req.oldItemIsDelete()) {
            rv = MutationStatus::Success;
        }

        if (req.isDelete()) {
            if (req.requestFailed()) {
                st.numDelFailure++;
            } else {
                st.delTimeHisto.add(req.getDelta());
            }

            logger->TRACE(
                    "MagmaKVStore::commitCallback(Del) {} errCode:{} rv:{}",
                    pendingReqs->front().getVbID(),
                    errCode,
                    to_string(rv));

            transactionCtx->deleteCallback(req.getItem(), rv);
        } else {
            auto mutationSetStatus = MutationSetResultState::Update;
            if (req.oldItemIsDelete()) {
                rv = MutationStatus::DocNotFound;
            }

            if (req.requestFailed()) {
                st.numSetFailure++;
                mutationSetStatus = MutationSetResultState::Failed;
            } else {
                st.writeTimeHisto.add(req.getDelta());
                st.writeSizeHisto.add(mutationSize);
            }

            if (rv == MutationStatus::DocNotFound) {
                mutationSetStatus = MutationSetResultState::Insert;
            }
            transactionCtx->setCallback(req.getItem(), mutationSetStatus);
            logger->TRACE(
                    "MagmaKVStore::commitCallback(Set) {} errCode:{} rv:{} "
                    "insertion:{}",
                    pendingReqs->front().getVbID(),
                    errCode,
                    to_string(rv),
                    to_string(mutationSetStatus));
        }
    }
}

void MagmaKVStore::rollback() {
    if (in_transaction) {
        in_transaction = false;
        transactionCtx.reset();
    }
}

// @TODO MB-38611: Correct ConcurrentWriteCompact so that BackgroundCompact is
//  not necessary. We should allow explicit compactions and writes concurrently.
//
// Magma does not allow explicit (ns_server/kv_engine) compactions to run
// concurrently with writes. It does however run implicit (background)
// compactions.
StorageProperties MagmaKVStore::getStorageProperties() {
    StorageProperties rv(StorageProperties::EfficientVBDump::Yes,
                         StorageProperties::EfficientVBDeletion::Yes,
                         StorageProperties::PersistedDeletion::No,
                         StorageProperties::EfficientGet::Yes,
                         StorageProperties::ConcurrentWriteCompact::No,
                         StorageProperties::BackgroundCompact::Yes,
                         StorageProperties::ByIdScan::No);
    return rv;
}

void MagmaKVStore::setMaxDataSize(size_t size) {
    const size_t memoryQuota = (size / configuration.getMaxShards()) *
                               configuration.getMagmaMemQuotaRatio();
    magma->SetMemoryQuota(memoryQuota);
}

// Note: This routine is only called during warmup. The caller
// can not make changes to the vbstate or it would cause race conditions.
std::vector<vbucket_state*> MagmaKVStore::listPersistedVbuckets() {
    std::vector<vbucket_state*> result;
    for (const auto& vb : cachedVBStates) {
        result.emplace_back(vb.get());
    }
    return result;
}

void MagmaKVStore::set(queued_item item) {
    if (!in_transaction) {
        throw std::logic_error(
                "MagmaKVStore::set: in_transaction must be true to perform a "
                "set operation.");
    }
    pendingReqs->emplace_back(std::move(item), logger);
}

GetValue MagmaKVStore::get(const DiskDocKey& key, Vbid vb) {
    return getWithHeader(key, vb, GetMetaOnly::No);
}

GetValue MagmaKVStore::getWithHeader(const KVFileHandle& kvFileHandle,
                                     const DiskDocKey& key,
                                     Vbid vbid,
                                     GetMetaOnly getMetaOnly) {
    return getWithHeader(key, vbid, getMetaOnly);
}

GetValue MagmaKVStore::getWithHeader(const DiskDocKey& key,
                                     Vbid vbid,
                                     GetMetaOnly getMetaOnly) {
    Slice keySlice = {reinterpret_cast<const char*>(key.data()), key.size()};
    Slice metaSlice;
    Slice valueSlice;
    Magma::FetchBuffer idxBuf;
    Magma::FetchBuffer seqBuf;
    bool found;

    auto start = std::chrono::steady_clock::now();

    Status status = magma->Get(
            vbid.get(), keySlice, idxBuf, seqBuf, metaSlice, valueSlice, found);

    if (logger->should_log(spdlog::level::TRACE)) {
        logger->TRACE(
                "MagmaKVStore::getWithHeader {} key:{} status:{} found:{} "
                "deleted:{}",
                vbid,
                cb::UserData{key.to_string()},
                status.String(),
                found,
                found ? isDeleted(metaSlice) : false);
    }

    if (!status) {
        logger->warn("MagmaKVStore::getWithHeader {} key:{} status:{}",
                     vbid,
                     cb::UserData{makeDiskDocKey(keySlice).to_string()},
                     status.String());
        st.numGetFailure++;
        return GetValue{NULL, magmaErr2EngineErr(status.ErrorCode())};
    }

    if (!found) {
        return GetValue{NULL, ENGINE_KEY_ENOENT};
    }

    // record stats
    st.readTimeHisto.add(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start));
    st.readSizeHisto.add(keySlice.Len() + metaSlice.Len() + valueSlice.Len());

    return makeGetValue(vbid, keySlice, metaSlice, valueSlice, getMetaOnly);
}

void MagmaKVStore::getMulti(Vbid vbid, vb_bgfetch_queue_t& itms) {

    for (auto& it : itms) {
        auto& key = it.first;
        Slice keySlice = {reinterpret_cast<const char*>(key.data()),
                          key.size()};
        Slice metaSlice;
        Slice valueSlice;
        Magma::FetchBuffer idxBuf;
        Magma::FetchBuffer seqBuf;
        bool found;

        Status status = magma->Get(vbid.get(),
                                   keySlice,
                                   idxBuf,
                                   seqBuf,
                                   metaSlice,
                                   valueSlice,
                                   found);

        if (logger->should_log(spdlog::level::TRACE)) {
            logger->TRACE(
                    "MagmaKVStore::getMulti {} key:{} status:{} found:{} "
                    "deleted:{}",
                    vbid,
                    cb::UserData{makeDiskDocKey(keySlice).to_string()},
                    status.String(),
                    found,
                    found ? isDeleted(metaSlice) : false);
        }

        auto errCode = magmaErr2EngineErr(status.ErrorCode(), found);
        vb_bgfetch_item_ctx_t& bg_itm_ctx = it.second;
        bg_itm_ctx.value.setStatus(errCode);

        if (found) {
            it.second.value = makeGetValue(vbid,
                                           keySlice,
                                           metaSlice,
                                           valueSlice,
                                           it.second.isMetaOnly);
            GetValue* rv = &it.second.value;

            for (auto& fetch : bg_itm_ctx.bgfetched_list) {
                fetch->value = rv;
                st.readTimeHisto.add(
                        std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() -
                                fetch->initTime));
                st.readSizeHisto.add(bg_itm_ctx.value.item->getKey().size() +
                                     bg_itm_ctx.value.item->getNBytes());
            }
        } else {
            if (!status) {
                logger->critical(
                        "MagmaKVStore::getMulti: {} key:{} status:{}, ",
                        vbid,
                        cb::UserData{makeDiskDocKey(keySlice).to_string()},
                        status.String());
                st.numGetFailure++;
            }
            for (auto& fetch : bg_itm_ctx.bgfetched_list) {
                fetch->value->setStatus(errCode);
            }
        }
    }
}

void MagmaKVStore::getRange(Vbid vbid,
                            const DiskDocKey& startKey,
                            const DiskDocKey& endKey,
                            const GetRangeCb& cb) {
    Slice startKeySlice = {reinterpret_cast<const char*>(startKey.data()),
                           startKey.size()};
    Slice endKeySlice = {reinterpret_cast<const char*>(endKey.data()),
                         endKey.size()};

    auto callback = [&](Slice& keySlice, Slice& metaSlice, Slice& valueSlice) {
        if (logger->should_log(spdlog::level::TRACE)) {
            logger->TRACE(
                    "MagmaKVStore::getRange callback {} key:{} seqno:{} "
                    "deleted:{}",
                    vbid,
                    cb::UserData{makeDiskDocKey(keySlice).to_string()},
                    getSeqNum(metaSlice),
                    isDeleted(metaSlice));
        }

        if (isDeleted(metaSlice)) {
            return;
        }
        auto rv = makeGetValue(vbid, keySlice, metaSlice, valueSlice);
        cb(std::move(rv));
    };

    if (logger->should_log(spdlog::level::TRACE)) {
        logger->TRACE("MagmaKVStore::getRange {} start:{} end:{}",
                      vbid,
                      cb::UserData{makeDiskDocKey(startKeySlice).to_string()},
                      cb::UserData{makeDiskDocKey(endKeySlice).to_string()});
    }

    auto status = magma->GetRange(
            vbid.get(), startKeySlice, endKeySlice, callback, true);
    if (!status) {
        logger->critical(
                "MagmaKVStore::getRange {} start:{} end:{} status:{}",
                vbid,
                cb::UserData{makeDiskDocKey(startKeySlice).to_string()},
                cb::UserData{makeDiskDocKey(endKeySlice).to_string()},
                status.String());
        st.numGetFailure++;
    }
}

void MagmaKVStore::reset(Vbid vbid) {
    auto vbstate = getVBucketState(vbid);
    if (!vbstate) {
        throw std::invalid_argument(
                "MagmaKVStore::reset: No entry in cached "
                "states for " +
                vbid.to_string());
    }

    Status status;
    Magma::KVStoreRevision kvsRev;
    std::tie(status, kvsRev) = magma->SoftDeleteKVStore(vbid.get());
    if (!status) {
        logger->critical(
                "MagmaKVStore::reset SoftDeleteKVStore failed. {} "
                "status:{}",
                vbid.to_string(),
                status.String());
    }
    // Even though SoftDeleteKVStore might have failed, go ahead and
    // call DeleteKVStore because it will remove the KVStore path
    // regardless if we think the kvstore is valid or not.
    status = magma->DeleteKVStore(vbid.get(), kvsRev);
    if (!status) {
        logger->critical(
                "MagmaKVStore::reset DeleteKVStore {} failed. status:{}",
                vbid,
                status.String());
    }

    vbstate->reset();
    auto& minfo = getMagmaInfo(vbid);
    minfo.reset();

    status = writeVBStateToDisk(vbid, *vbstate, minfo);
    if (!status) {
        throw std::runtime_error("MagmaKVStore::reset writeVBStateToDisk " +
                                 vbid.to_string() +
                                 " failed. Status:" + status.String());
    }
}

void MagmaKVStore::del(queued_item item) {
    if (!in_transaction) {
        throw std::logic_error(
                "MagmaKVStore::del: in_transaction must be true to perform a "
                "delete operation.");
    }
    pendingReqs->emplace_back(std::move(item), logger);
}

void MagmaKVStore::delVBucket(Vbid vbid, uint64_t vb_version) {
    auto status = magma->DeleteKVStore(
            vbid.get(), static_cast<Magma::KVStoreRevision>(vb_version));
    logger->info("MagmaKVStore::delVBucket DeleteKVStore {} {}. status:{}",
                 vbid,
                 vb_version,
                 status.String());
}

void MagmaKVStore::prepareToCreateImpl(Vbid vbid) {
    if (magma->KVStoreExists(vbid.get())) {
        throw std::logic_error("MagmaKVStore::prepareToCreateImpl " +
                               vbid.to_string() +
                               " Can't call prepareToCreate before calling" +
                               " prepareToDelete on an existing kvstore.");
    }
    auto vbstate = getVBucketState(vbid);
    if (vbstate) {
        vbstate->reset();
    }
    auto& minfo = getMagmaInfo(vbid);
    minfo.reset();
    minfo.kvstoreRev++;
    logger->info("MagmaKVStore::prepareToCreateImpl {} kvstoreRev:{}",
                 vbid,
                 minfo.kvstoreRev);
}

uint64_t MagmaKVStore::prepareToDeleteImpl(Vbid vbid) {
    if (magma->KVStoreExists(vbid.get())) {
        Status status;
        Magma::KVStoreRevision kvsRev;
        std::tie(status, kvsRev) = magma->SoftDeleteKVStore(vbid.get());
        if (!status) {
            logger->critical(
                    "MagmaKVStore::prepareToDeleteImpl SoftDeleteKVStore "
                    "failed. {} status:{}",
                    vbid,
                    status.String());
        }
        auto& minfo = getMagmaInfo(vbid);
        logger->info("MagmaKVStore::prepareToDeleteImpl {} kvstoreRev:{}",
                     vbid,
                     minfo.kvstoreRev);
        return static_cast<uint64_t>(minfo.kvstoreRev);
    }
    return 0;
}

// Note: It is assumed this can only be called from bg flusher thread or
// there will be issues with writes coming from multiple threads.
bool MagmaKVStore::snapshotVBucket(Vbid vbid, const vbucket_state& newVBState) {
    // Taking a copy of the MagmaInfo
    auto& minfo = getMagmaInfo(vbid);

    if (logger->should_log(spdlog::level::TRACE)) {
        auto j = encodeVBState(newVBState, minfo);
        logger->TRACE("MagmaKVStore::snapshotVBucket {} newVBState:{}",
                      vbid,
                      j.dump());
    }

    auto start = std::chrono::steady_clock::now();

    if (updateCachedVBState(vbid, newVBState)) {
        auto currVBState = getVBucketState(vbid);
        auto status = writeVBStateToDisk(vbid, *currVBState, minfo);
        if (!status) {
            return false;
        }
    }

    st.snapshotHisto.add(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start));

    return true;
}

std::unique_ptr<Item> MagmaKVStore::makeItem(Vbid vb,
                                             const Slice& keySlice,
                                             const Slice& metaSlice,
                                             const Slice& valueSlice,
                                             GetMetaOnly getMetaOnly) {
    auto key = makeDiskDocKey(keySlice);
    auto& meta = *reinterpret_cast<const magmakv::MetaData*>(metaSlice.Data());

    bool includeValue = getMetaOnly == GetMetaOnly::No && meta.valueSize;

    auto item =
            std::make_unique<Item>(key.getDocKey(),
                                   meta.flags,
                                   meta.exptime,
                                   includeValue ? valueSlice.Data() : nullptr,
                                   includeValue ? meta.valueSize : 0,
                                   meta.datatype,
                                   meta.cas,
                                   meta.bySeqno,
                                   vb,
                                   meta.revSeqno);

    if (meta.deleted) {
        item->setDeleted(static_cast<DeleteSource>(meta.deleteSource));
    }

    switch (meta.getOperation()) {
    case magmakv::MetaData::Operation::Mutation:
        // Item already defaults to Mutation - nothing else to do.
        return item;
    case magmakv::MetaData::Operation::PreparedSyncWrite:
        // From disk we return a zero (infinite) timeout; as this could
        // refer to an already-committed SyncWrite and hence timeout
        // must be ignored.
        item->setPendingSyncWrite({meta.getDurabilityLevel(),
                                   cb::durability::Timeout::Infinity()});
        return item;
    case magmakv::MetaData::Operation::CommittedSyncWrite:
        item->setCommittedviaPrepareSyncWrite();
        item->setPrepareSeqno(meta.prepareSeqno);
        return item;
    case magmakv::MetaData::Operation::Abort:
        item->setAbortSyncWrite();
        item->setPrepareSeqno(meta.prepareSeqno);
        return item;
    }

    throw std::logic_error("MagmaKVStore::makeItem unexpected operation:" +
                           meta.to_string(meta.getOperation()));
}

GetValue MagmaKVStore::makeGetValue(Vbid vb,
                                    const Slice& keySlice,
                                    const Slice& metaSlice,
                                    const Slice& valueSlice,
                                    GetMetaOnly getMetaOnly) {
    return GetValue(makeItem(vb, keySlice, metaSlice, valueSlice, getMetaOnly),
                    ENGINE_SUCCESS,
                    -1,
                    0);
}

int MagmaKVStore::saveDocs(VB::Commit& commitData, kvstats_ctx& kvctx) {
    uint64_t ninserts = 0;
    uint64_t ndeletes = 0;
    int64_t lastSeqno = 0;

    auto vbid = transactionCtx->vbid;

    // Start a magma CommitBatch.
    // This is an atomic batch of items... all or nothing.
    std::unique_ptr<Magma::CommitBatch> batch;
    auto& minfo = getMagmaInfo(vbid);
    Status status = magma->NewCommitBatch(
            vbid.get(),
            batch,
            static_cast<Magma::KVStoreRevision>(minfo.kvstoreRev));
    if (!status) {
        logger->warn(
                "MagmaKVStore::saveDocs NewCommitBatch failed {} status:{}",
                vbid,
                status.String());
        return status.ErrorCode();
    }

    auto begin = std::chrono::steady_clock::now();

    for (auto& req : *pendingReqs) {
        Slice key = Slice{req.getKey(), req.getKeyLen()};
        auto docMeta = req.getDocMeta();
        Slice meta = Slice{reinterpret_cast<char*>(&docMeta),
                           sizeof(magmakv::MetaData)};
        Slice value = Slice{req.getBodyData(), req.getBodySize()};

        if (req.getDocMeta().bySeqno > lastSeqno) {
            lastSeqno = req.getDocMeta().bySeqno;
        }

        bool found{false};
        bool tombstone{false};
        status = batch->Set(key, meta, value, &found, &tombstone);
        if (!status) {
            logger->warn("MagmaKVStore::saveDocs: Set {} status:{}",
                         vbid,
                         status.String());
            req.markRequestFailed();
            continue;
        }

        if (logger->should_log(spdlog::level::TRACE)) {
            logger->TRACE(
                    "MagmaKVStore::saveDocs {} key:{} seqno:{} delete:{} "
                    "found:{} "
                    "tombstone:{}",
                    vbid,
                    cb::UserData{makeDiskDocKey(key).to_string()},
                    req.getDocMeta().bySeqno,
                    req.isDelete(),
                    found,
                    tombstone);
        }

        auto diskDocKey = makeDiskDocKey(key);
        auto docKey = diskDocKey.getDocKey();

        if (found) {
            req.markOldItemExists();
            if (tombstone) {
                req.markOldItemIsDelete();
                // Old item is a delete and new is an insert.
                if (!req.isDelete()) {
                    ninserts++;
                    if (diskDocKey.isCommitted()) {
                        commitData.collections.incrementDiskCount(docKey);
                    } else {
                        kvctx.onDiskPrepareDelta++;
                    }
                }
            } else if (req.isDelete()) {
                // Old item is insert and new is delete.
                ndeletes++;
                if (diskDocKey.isCommitted()) {
                    commitData.collections.decrementDiskCount(docKey);
                } else {
                    kvctx.onDiskPrepareDelta--;
                }
            }
        } else {
            // Old item doesn't exist and new is an insert.
            if (!req.isDelete()) {
                ninserts++;
                if (diskDocKey.isCommitted()) {
                    commitData.collections.incrementDiskCount(docKey);
                } else {
                    kvctx.onDiskPrepareDelta++;
                }
            }
        }

        if (req.oldItemExists()) {
            if (!req.oldItemIsDelete()) {
                // If we are replacing the item...
                kvctx.keyWasOnDisk.insert(diskDocKey);
            }
        }

        commitData.collections.setPersistedHighSeqno(
                docKey, req.getDocMeta().bySeqno, int(req.isDelete()));
    }

    commitData.collections.saveCollectionStats(
            std::bind(&MagmaKVStore::saveCollectionStats,
                      this,
                      std::ref(*(batch.get())),
                      std::placeholders::_1,
                      std::placeholders::_2));

    auto& vbstate = commitData.proposedVBState;
    vbstate.highSeqno = lastSeqno;
    vbstate.onDiskPrepares += kvctx.onDiskPrepareDelta;
    auto& magmaInfo = getMagmaInfo(vbid);

    magmaInfo.docCount += ninserts - ndeletes;
    magmaInfo.persistedDeletes += ndeletes;

    // Write out current vbstate to the CommitBatch.
    status = writeVBStateToDisk(vbid, *(batch.get()), vbstate, magmaInfo);
    if (!status) {
        return status.ErrorCode();
    }

    if (collectionsMeta.needsCommit) {
        status = updateCollectionsMeta(
                vbid, *(batch.get()), commitData.collections);
        if (!status) {
            logger->warn(
                    "MagmaKVStore::saveDocs: updateCollectionsMeta status:{}",
                    status.String());
        }
    }

    // Done with the CommitBatch so add in the endTxn and create
    // a snapshot so that documents can not be read.
    status = magma->ExecuteCommitBatch(std::move(batch));
    if (!status) {
        logger->warn("MagmaKVStore::saveDocs: ExecuteCommitBatch {} status:{}",
                     vbid,
                     status.String());
    } else {
        // Flush the WAL to disk.
        // If the shard writeCache is full, this will trigger all the
        // KVStores to persist their memtables.
        status = magma->SyncCommitBatches(commitPointEveryBatch);
        if (!status) {
            logger->warn(
                    "MagmaKVStore::saveDocs: SyncCommitBatches {} status:{}",
                    vbid,
                    status.String());
        }
    }

    st.commitHisto.add(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - begin));

    return status.ErrorCode();
}

MagmaScanContext::MagmaScanContext(
        std::unique_ptr<StatusCallback<GetValue>> cb,
        std::unique_ptr<StatusCallback<CacheLookup>> cl,
        Vbid vb,
        std::unique_ptr<KVFileHandle> handle,
        int64_t start,
        int64_t end,
        uint64_t purgeSeqno,
        DocumentFilter _docFilter,
        ValueFilter _valFilter,
        uint64_t _documentCount,
        const vbucket_state& vbucketState,
        const std::vector<Collections::KVStore::DroppedCollection>&
                droppedCollections,
        std::unique_ptr<magma::Magma::SeqIterator> itr)
    : BySeqnoScanContext(std::move(cb),
                         std::move(cl),
                         vb,
                         std::move(handle),
                         start,
                         end,
                         purgeSeqno,
                         _docFilter,
                         _valFilter,
                         _documentCount,
                         vbucketState,
                         droppedCollections),
      itr(std::move(itr)) {
}

std::unique_ptr<BySeqnoScanContext> MagmaKVStore::initBySeqnoScanContext(
        std::unique_ptr<StatusCallback<GetValue>> cb,
        std::unique_ptr<StatusCallback<CacheLookup>> cl,
        Vbid vbid,
        uint64_t startSeqno,
        DocumentFilter options,
        ValueFilter valOptions) {
    auto handle = makeFileHandle(vbid);

    auto readState = readVBStateFromDisk(vbid);
    if (!readState.status.IsOK()) {
        logger->warn(
                "MagmaKVStore::initBySeqnoScanContext {} failed to read "
                "vbstate "
                "from disk. Status:{}",
                vbid,
                readState.status.String());
        return nullptr;
    }

    uint64_t highSeqno = readState.vbstate.highSeqno;
    uint64_t purgeSeqno = readState.vbstate.purgeSeqno;
    uint64_t nDocsToRead = highSeqno - startSeqno + 1;

    auto collectionsManifest = getDroppedCollections(vbid);

    if (logger->should_log(spdlog::level::info)) {
        std::string docFilter;
        switch (options) {
        case DocumentFilter::ALL_ITEMS:
            docFilter = "ALL_ITEMS";
            break;
        case DocumentFilter::NO_DELETES:
            docFilter = "NO_DELETES";
            break;
        case DocumentFilter::ALL_ITEMS_AND_DROPPED_COLLECTIONS:
            docFilter = "ALL_ITEMS_AND_DROPPED_COLLECTIONS";
            break;
        }
        if (docFilter.size() == 0) {
            throw std::logic_error(
                    "MagmaKVStore::initBySeqnoScanContext Unknown "
                    "DocumentFilter:" +
                    std::to_string(static_cast<int>(options)));
        }

        std::string valFilter;
        switch (valOptions) {
        case ValueFilter::KEYS_ONLY:
            valFilter = "KEYS_ONLY";
            break;
        case ValueFilter::VALUES_COMPRESSED:
            valFilter = "VALUES_COMPRESSED";
            break;
        case ValueFilter::VALUES_DECOMPRESSED:
            valFilter = "VALUES_DECOMPRESSED";
            break;
        }
        if (valFilter.size() == 0) {
            throw std::logic_error(
                    "MagmaKVStore::initBySeqnoScanContext Unknown "
                    "ValueFilter:" +
                    std::to_string(static_cast<int>(valOptions)));
        }

        logger->info(
                "MagmaKVStore::initBySeqnoScanContext {} seqno:{} endSeqno:{}"
                " purgeSeqno:{} nDocsToRead:{} docFilter:{} valFilter:{}",
                vbid,
                startSeqno,
                highSeqno,
                purgeSeqno,
                nDocsToRead,
                docFilter,
                valFilter);
    }

    auto itr = magma->NewSeqIterator(vbid.get());
    if (!itr) {
        logger->warn(
                "MagmaKVStore::initScanContext {} Failed to get magma seq "
                "iterator",
                vbid);
        return nullptr;
    }

    auto mctx = std::make_unique<MagmaScanContext>(std::move(cb),
                                                   std::move(cl),
                                                   vbid,
                                                   std::move(handle),
                                                   startSeqno,
                                                   highSeqno,
                                                   purgeSeqno,
                                                   options,
                                                   valOptions,
                                                   nDocsToRead,
                                                   readState.vbstate,
                                                   collectionsManifest,
                                                   std::move(itr));
    return mctx;
}

std::unique_ptr<ByIdScanContext> MagmaKVStore::initByIdScanContext(
        std::unique_ptr<StatusCallback<GetValue>> cb,
        std::unique_ptr<StatusCallback<CacheLookup>> cl,
        Vbid vbid,
        const std::vector<ByIdRange>& ranges,
        DocumentFilter options,
        ValueFilter valOptions) {
    throw std::runtime_error(
            "MagmaKVStore::initByIdScanContext (id scan) unimplemented");
    return {};
}

scan_error_t MagmaKVStore::scan(BySeqnoScanContext& ctx) {
    if (ctx.lastReadSeqno == ctx.maxSeqno) {
        logger->TRACE("MagmaKVStore::scan {} lastReadSeqno:{} == maxSeqno:{}",
                      ctx.vbid,
                      ctx.lastReadSeqno,
                      ctx.maxSeqno);
        return scan_success;
    }

    auto startSeqno = ctx.startSeqno;
    if (ctx.lastReadSeqno != 0) {
        startSeqno = ctx.lastReadSeqno + 1;
    }

    GetMetaOnly isMetaOnly = ctx.valFilter == ValueFilter::KEYS_ONLY
                                     ? GetMetaOnly::Yes
                                     : GetMetaOnly::No;
    bool onlyKeys = ctx.valFilter == ValueFilter::KEYS_ONLY;

    auto& mctx = dynamic_cast<MagmaScanContext&>(ctx);
    for (mctx.itr->Seek(startSeqno, ctx.maxSeqno); mctx.itr->Valid();
         mctx.itr->Next()) {
        Slice keySlice, metaSlice, valSlice;
        uint64_t seqno;
        mctx.itr->GetRecord(keySlice, metaSlice, valSlice, seqno);

        if (keySlice.Len() > std::numeric_limits<uint16_t>::max()) {
            throw std::invalid_argument(
                    "MagmaKVStore::scan: "
                    "key length " +
                    std::to_string(keySlice.Len()) + " > " +
                    std::to_string(std::numeric_limits<uint16_t>::max()));
        }

        auto diskKey = makeDiskDocKey(keySlice);

        if (isDeleted(metaSlice) &&
            ctx.docFilter == DocumentFilter::NO_DELETES) {
            logger->TRACE(
                    "MagmaKVStore::scan SKIPPED(Deleted) {} key:{} seqno:{}",
                    ctx.vbid,
                    cb::UserData{diskKey.to_string()},
                    seqno);
            continue;
        }

        auto docKey = diskKey.getDocKey();

        // Determine if the key is logically deleted, if it is we skip the key
        // Note that system event keys (like create scope) are never skipped
        // here
        if (!docKey.getCollectionID().isSystem()) {
            if (ctx.docFilter !=
                DocumentFilter::ALL_ITEMS_AND_DROPPED_COLLECTIONS) {
                if (ctx.collectionsContext.isLogicallyDeleted(docKey, seqno)) {
                    ctx.lastReadSeqno = seqno;
                    logger->TRACE(
                            "MagmaKVStore::scan SKIPPED(Collection Deleted) {} "
                            "key:{} "
                            "seqno:{}",
                            ctx.vbid,
                            cb::UserData{diskKey.to_string()},
                            seqno);
                    continue;
                }
            }

            CacheLookup lookup(diskKey, seqno, ctx.vbid);

            ctx.lookup->callback(lookup);
            if (ctx.lookup->getStatus() == ENGINE_KEY_EEXISTS) {
                ctx.lastReadSeqno = seqno;
                logger->TRACE(
                        "MagmaKVStore::scan SKIPPED(ENGINE_KEY_EEXISTS) {} "
                        "key:{} seqno:{}",
                        ctx.vbid,
                        cb::UserData{diskKey.to_string()},
                        seqno);
                continue;
            } else if (ctx.lookup->getStatus() == ENGINE_ENOMEM) {
                logger->warn(
                        "MagmaKVStore::scan lookup->callback {} "
                        "key:{} returned ENGINE_ENOMEM",
                        ctx.vbid,
                        cb::UserData{diskKey.to_string()});
                return scan_again;
            }
        }

        logger->TRACE(
                "MagmaKVStore::scan {} key:{} seqno:{} deleted:{} expiry:{} "
                "compressed:{}",
                ctx.vbid,
                diskKey.to_string(),
                seqno,
                isDeleted(metaSlice),
                getExpiryTime(metaSlice),
                isCompressed(metaSlice));

        auto itm =
                makeItem(ctx.vbid, keySlice, metaSlice, valSlice, isMetaOnly);

        // When we are suppose to return the values as compressed AND
        // the value isn't compressed, we need to compress the value.
        if (ctx.valFilter == ValueFilter::VALUES_COMPRESSED &&
            !isCompressed(metaSlice)) {
            if (!itm->compressValue(true)) {
                logger->warn(
                        "MagmaKVStore::scan failed to compress value - {} "
                        "key:{} "
                        "seqno:{}",
                        ctx.vbid,
                        cb::UserData{diskKey.to_string()},
                        seqno);
                continue;
            }
        }

        GetValue rv(std::move(itm), ENGINE_SUCCESS, -1, onlyKeys);
        ctx.callback->callback(rv);
        auto callbackStatus = ctx.callback->getStatus();
        if (callbackStatus == ENGINE_ENOMEM) {
            logger->warn(
                    "MagmaKVStore::scan callback {} "
                    "key:{} returned ENGINE_ENOMEM",
                    ctx.vbid,
                    cb::UserData{diskKey.to_string()});
            return scan_again;
        }
        ctx.lastReadSeqno = seqno;
    }

    return scan_success;
}

scan_error_t MagmaKVStore::scan(ByIdScanContext& ctx) {
    throw std::runtime_error(
            "MagmaKVStore::initScanContext (id scan) unimplemented");
    return scan_failed;
}

vbucket_state* MagmaKVStore::getVBucketState(Vbid vbid) {

    // Have to assume the caller is protecting the vbstate
    auto vbstate = cachedVBStates[vbid.get()].get();
    if (vbstate && logger->should_log(spdlog::level::TRACE)) {
        auto& minfo = getMagmaInfo(vbid);
        auto j = encodeVBState(*vbstate, minfo);
        logger->TRACE(
                "MagmaKVStore::getVBucketState {} vbstate:{}", vbid, j.dump());
    }
    return vbstate;
}

MagmaKVStore::DiskState MagmaKVStore::readVBStateFromDisk(Vbid vbid) {
    Status status;
    std::string valString;
    std::tie(status, valString) = readLocalDoc(vbid, vbstateKey);

    if (!status.IsOK()) {
        return {status, {}, {}};
    }

    nlohmann::json j;

    try {
        j = nlohmann::json::parse(valString);
    } catch (const nlohmann::json::exception& e) {
        return {Status("MagmaKVStore::readVBStateFromDisk failed - vbucket( " +
                       std::to_string(vbid.get()) + ") " +
                       " Failed to parse the vbstate json doc: " + valString +
                       ". Reason: " + e.what()),
                {},
                {}};
    }

    logger->TRACE(
            "MagmaKVStore::readVBStateFromDisk {} vbstate:{}", vbid, j.dump());

    vbucket_state vbstate = j;
    MagmaInfo magmaInfo = {
            std::stoull(j.at("doc_count").get<std::string>()),
            std::stoull(j.at("persisted_deletes").get<std::string>()),
            std::stoull(j.at("kvstore_revision").get<std::string>())};

    return {status, vbstate, magmaInfo};
}

magma::Status MagmaKVStore::loadVBStateCache(Vbid vbid) {
    const auto readState = readVBStateFromDisk(vbid);
    if (!readState.status.IsOK()) {
        logger->error("MagmaKVStore::loadVBStateCache {} failed. {}",
                      vbid,
                      readState.status.String());
        return readState.status;
    }

    cachedVBStates[vbid.get()] =
            std::make_unique<vbucket_state>(readState.vbstate);
    cachedMagmaInfo[vbid.get()] =
            std::make_unique<MagmaInfo>(readState.magmaInfo);

    return Status::OK();
}

magma::Status MagmaKVStore::writeVBStateToDisk(Vbid vbid,
                                               Magma::CommitBatch& commitBatch,
                                               const vbucket_state& vbs,
                                               const MagmaInfo& minfo) {
    auto jstr = encodeVBState(vbs, minfo).dump();
    logger->TRACE("MagmaKVStore::writeVBStateToDisk {} vbstate:{}", vbid, jstr);
    Slice valSlice(jstr);
    return setLocalDoc(commitBatch, vbstateKey, valSlice);
}

std::pair<Status, std::string> MagmaKVStore::readLocalDoc(
        Vbid vbid, const Slice& keySlice) {
    Slice valSlice;
    Magma::FetchBuffer valBuf;
    bool found{false};
    magma::Status retStatus = Status::OK();
    std::string valString;

    auto status =
            magma->GetLocal(vbid.get(), keySlice, valBuf, valSlice, found);
    // If the kvstore hasn't been created yet, we get a NotExists error.
    // We don't know if its been created.
    if (!status && status.ErrorCode() != Status::Code::NotExists) {
        retStatus = magma::Status(
                status.ErrorCode(),
                "MagmaKVStore::readLocalDoc vbid:" + vbid.to_string() +
                        " key:" + keySlice.ToString() + " " + status.String());
    } else {
        if (!found) {
            retStatus = magma::Status(
                    magma::Status::Code::NotFound,
                    "MagmaKVStore::readLocalDoc(vbid:" + vbid.to_string() +
                            " key:" + keySlice.ToString() + " not found.");
        } else if (logger->should_log(spdlog::level::TRACE)) {
            logger->TRACE("MagmaKVStore::readLocalDoc {} key:{} valueLen:{}",
                          vbid,
                          keySlice.ToString(),
                          valString.length());
        }
    }
    return std::make_pair(retStatus, valSlice.ToString());
}

Status MagmaKVStore::setLocalDoc(Magma::CommitBatch& commitBatch,
                                 const Slice& keySlice,
                                 const Slice& valSlice) {
    if (logger->should_log(spdlog::level::TRACE)) {
        logger->TRACE("MagmaKVStore::setLocalDoc {} key:{} valueLen:{}",
                      commitBatch.GetkvID(),
                      keySlice.ToString(),
                      valSlice.Len());
    }
    return commitBatch.SetLocal(keySlice, valSlice);
}

Status MagmaKVStore::deleteLocalDoc(Magma::CommitBatch& commitBatch,
                                    const Slice& keySlice) {
    if (logger->should_log(spdlog::level::TRACE)) {
        logger->TRACE("MagmaKVStore::deleteLocalDoc {} key:{}",
                      commitBatch.GetkvID(),
                      keySlice.ToString());
    }
    return commitBatch.DeleteLocal(keySlice);
}

nlohmann::json MagmaKVStore::encodeVBState(const vbucket_state& vbstate,
                                           const MagmaInfo& magmaInfo) const {
    nlohmann::json j = vbstate;
    j["doc_count"] = std::to_string(static_cast<uint64_t>(magmaInfo.docCount));
    j["persisted_deletes"] =
            std::to_string(static_cast<uint64_t>(magmaInfo.persistedDeletes));
    j["kvstore_revision"] =
            std::to_string(static_cast<uint64_t>(magmaInfo.kvstoreRev));
    return j;
}

ENGINE_ERROR_CODE MagmaKVStore::magmaErr2EngineErr(Status::Code err,
                                                   bool found) {
    if (!found) {
        return ENGINE_KEY_ENOENT;
    }
    // This routine is intended to mimic couchErr2EngineErr.
    // Since magma doesn't have a memory allocation error, all magma errors
    // get translated into ENGINE_TMPFAIL.
    if (err == Status::Code::Ok) {
        return ENGINE_SUCCESS;
    }
    return ENGINE_TMPFAIL;
}

Vbid MagmaKVStore::getDBFileId(const cb::mcbp::Request& req) {
    return req.getVBucket();
}

size_t MagmaKVStore::getItemCount(Vbid vbid) {
    auto readState = readVBStateFromDisk(vbid);
    if (!readState.status.IsOK()) {
        logger->warn(
                "MagmaKVStore::getItemCount {} failed to read "
                "vbstate "
                "from disk. Status:{}",
                vbid,
                readState.status.String());
        return 0;
    }
    return readState.magmaInfo.docCount;
}

ENGINE_ERROR_CODE MagmaKVStore::getAllKeys(
        Vbid vbid,
        const DiskDocKey& startKey,
        uint32_t count,
        std::shared_ptr<Callback<const DiskDocKey&>> cb) {

    Slice startKeySlice = {reinterpret_cast<const char*>(startKey.data()),
                           startKey.size()};

    // The magma GetRange API takes a start and end key. getAllKeys only
    // supplies a start key and count of the number of keys it wants.
    // When endKeySlice is uninitialized (as it is here), the
    // endKeySlice.Len() will be 0 which tells GetRange to ignore it.
    Slice endKeySlice;

    uint32_t processed = 0;

    auto callback = [&](Slice& keySlice, Slice& metaSlice, Slice& valueSlice) {
        if (logger->should_log(spdlog::level::TRACE)) {
            logger->TRACE(
                    "MagmaKVStore::getAllKeys callback {} key:{} seqno:{} "
                    "deleted:{}",
                    vbid,
                    cb::UserData{makeDiskDocKey(keySlice).to_string()},
                    getSeqNum(metaSlice),
                    isDeleted(metaSlice));
        }

        if (isDeleted(metaSlice)) {
            return;
        }
        auto retKey = makeDiskDocKey(keySlice);
        cb->callback(retKey);
        processed++;
    };

    if (logger->should_log(spdlog::level::TRACE)) {
        logger->TRACE("MagmaKVStore::getAllKeys {} start:{} count:{})",
                      vbid,
                      cb::UserData{startKey.to_string()},
                      count);
    }

    auto status = magma->GetRange(
            vbid.get(), startKeySlice, endKeySlice, callback, false, count);
    if (!status) {
        logger->critical("MagmaKVStore::getAllKeys {} startKey:{} status:{}",
                         vbid,
                         cb::UserData{startKey.to_string()},
                         status.String());
    }

    if (!status) {
        return magmaErr2EngineErr(status.ErrorCode(), true);
    }

    return ENGINE_SUCCESS;
}

bool MagmaKVStore::compactDB(compaction_ctx* ctx) {
    auto res = compactDBInternal(ctx);

    if (!res) {
        st.numCompactionFailure++;
    }

    return res;
}

bool MagmaKVStore::compactDBInternal(compaction_ctx* ctx) {
    std::chrono::steady_clock::time_point start =
            std::chrono::steady_clock::now();

    logger->info(
            "MagmaKVStore::compactDBInternal: {} purge_before_ts:{} "
            "purge_before_seq:{} drop_deletes:{} purgeSeq:{} "
            "retain_erroneous_tombstones:{}",
            ctx->compactConfig.db_file_id,
            ctx->compactConfig.purge_before_ts,
            ctx->compactConfig.purge_before_seq,
            ctx->compactConfig.drop_deletes,
            ctx->compactConfig.purgeSeq,
            ctx->compactConfig.retain_erroneous_tombstones);

    Vbid vbid = ctx->compactConfig.db_file_id;

    auto status = magma->Sync(true);
    if (!status) {
        logger->warn("MagmaKVStore::compactDBInternal: Sync failed. {}",
                     ctx->compactConfig.db_file_id);
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(compactionCtxMutex);
        compaction_ctxList[vbid.get()] =
                std::make_shared<MagmaCompactionCtx>(ctx);
    }

    auto dropped = getDroppedCollections(vbid);
    ctx->eraserContext =
            std::make_unique<Collections::VB::EraserContext>(dropped);

    auto diskState = readVBStateFromDisk(vbid);
    if (!diskState.status) {
        throw std::runtime_error(
                "MagmaKVStore::compactDBInternal: trying to run compaction "
                "on " +
                vbid.to_string() +
                " but can't read vbstate. Status:" + diskState.status.String());
    }
    ctx->highCompletedSeqno = diskState.vbstate.persistedCompletedSeqno;

    // If there aren't any collections to drop, this compaction is likely
    // being called from a test because kv_engine shouldn't call compactDB
    // to compact levels, magma takes care of that already.
    if (dropped.empty()) {
        // Perform synchronous compaction.
        status = magma->CompactKVStore(vbid.get(), Magma::StoreType::Key);
        if (!status) {
            logger->warn("MagmaKVStore::compactDBInternal: Sync failed for {} ",
                         vbid);
            return false;
        }
    } else {
        for (auto& dc : dropped) {
            std::string keyString =
                    Collections::makeCollectionIdIntoString(dc.collectionId);
            Slice keySlice{keyString};

            if (logger->should_log(spdlog::level::TRACE)) {
                auto docKey = makeDiskDocKey(keySlice);
                logger->TRACE(
                        "MagmaKVStore::compactDBInternal: PurgeKVStore "
                        "{} key:{}",
                        vbid,
                        cb::UserData{docKey.to_string()});
            }

            // This will find all the keys with a prefix of the collection ID
            // and drop them.
            status = magma->PurgeKVStore(
                    vbid.get(), keySlice, keySlice, nullptr, true);
            if (!status) {
                logger->warn(
                        "MagmaKVStore::compactDBInternal: PurgeKVStore {} "
                        "key:{} failed status:{}",
                        vbid,
                        cb::UserData{makeDiskDocKey(keySlice).to_string()},
                        status.String());
            }

            // We've finish processing this collection.
            // Create a SystemEvent key for the collection and process it.
            auto collectionKey =
                    SystemEventFactory::makeCollectionEventKey(dc.collectionId);

            keySlice = {reinterpret_cast<const char*>(collectionKey.data()),
                        collectionKey.size()};

            auto key = makeDiskDocKey(keySlice);
            if (logger->should_log(spdlog::level::TRACE)) {
                logger->TRACE(
                        "MagmaKVStore::compactDBInternal: "
                        "processEndOfCollection {} key:{}",
                        vbid,
                        cb::UserData{key.to_string()});
            }

            ctx->eraserContext->processEndOfCollection(key.getDocKey(),
                                                       SystemEvent::Collection);
        }
    }

    // Need to save off new vbstate and possibly collections manifest.
    // Start a new CommitBatch
    std::unique_ptr<Magma::CommitBatch> batch;

    status = magma->NewCommitBatch(
            vbid.get(),
            batch,
            static_cast<Magma::KVStoreRevision>(getMagmaInfo(vbid).kvstoreRev));
    if (!status) {
        logger->warn(
                "MagmaKVStore::compactDBInternal: failed creating batch for {} "
                "status:{}",
                vbid,
                status.String());
        return false;
    }

    auto vbstate = getVBucketState(vbid);
    auto& minfo = getMagmaInfo(vbid);
    vbstate->onDiskPrepares -= ctx->stats.preparesPurged;
    vbstate->purgeSeqno = ctx->max_purged_seq;
    minfo.docCount -=
            ctx->stats.collectionsItemsPurged + ctx->stats.preparesPurged;

    // Make our completion callback before writing the new file. We should
    // update our in memory state before we finalize on disk state so that we
    // don't have to worry about race conditions with things like the purge
    // seqno.
    if (ctx->completionCallback) {
        ctx->completionCallback(*ctx);
    }

    writeVBStateToDisk(vbid, *(batch.get()), *vbstate, minfo);

    if (ctx->eraserContext->needToUpdateCollectionsMetadata()) {
        status = deleteLocalDoc(*(batch.get()), droppedCollectionsKey);
        if (!status) {
            logger->warn(
                    "MagmaKVStore::compactDBInternal: magma::deleteLocalDoc "
                    "{} update dropped collections failed status:{}",
                    vbid,
                    status.String());
        }
    }

    // Write & Sync the CommitBatch
    status = magma->ExecuteCommitBatch(std::move(batch));
    if (!status) {
        logger->warn(
                "MagmaKVStore::compactDBInternal: magma::ExecuteCommitBatch "
                "{} status:{}",
                vbid,
                status.String());
    } else {
        status = magma->SyncCommitBatches(commitPointEveryBatch);
        if (!status) {
            logger->warn(
                    "MagmaKVStore::compactDBInternal: magma::SyncCommitBatches "
                    "{} status:{}",
                    vbid,
                    status.String());
        }
    }

    st.compactHisto.add(std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start));

    logger->TRACE(
            "MagmaKVStore::compactDBInternal: max_purged_seq:{}"
            " collectionsItemsPurged:{}"
            " collectionsDeletedItemsPurged:{}"
            " tombstonesPurged:{}"
            " preparesPurged:{}",
            ctx->max_purged_seq,
            ctx->stats.collectionsItemsPurged,
            ctx->stats.collectionsDeletedItemsPurged,
            ctx->stats.tombstonesPurged,
            ctx->stats.preparesPurged);

    {
        std::lock_guard<std::mutex> lock(compactionCtxMutex);
        compaction_ctxList[vbid.get()].reset();
    }

    return true;
}

std::unique_ptr<KVFileHandle> MagmaKVStore::makeFileHandle(Vbid vbid) {
    return std::make_unique<MagmaKVFileHandle>(vbid);
}

RollbackResult MagmaKVStore::rollback(Vbid vbid,
                                      uint64_t rollbackSeqno,
                                      std::unique_ptr<RollbackCB> callback) {
    logger->info("MagmaKVStore::rollback {} seqno:{}", vbid, rollbackSeqno);

    Magma::FetchBuffer idxBuf;
    Magma::FetchBuffer seqBuf;
    auto cacheLookup = std::make_shared<NoLookupCallback>();
    callback->setKVFileHandle(makeFileHandle(vbid));

    auto dropped = getDroppedCollections(vbid);
    auto eraserContext =
            std::make_unique<Collections::VB::EraserContext>(dropped);

    auto keyCallback =
            [this, ec = eraserContext.get(), cb = callback.get(), vbid](
                    const Slice& keySlice,
                    const uint64_t seqno,
                    const Slice& metaSlice) {
                auto diskKey = makeDiskDocKey(keySlice);
                auto docKey = diskKey.getDocKey();
                if (!docKey.getCollectionID().isSystem() &&
                    ec->isLogicallyDeleted(docKey, seqno)) {
                    return;
                }
                auto rv = this->makeGetValue(
                        vbid, keySlice, metaSlice, Slice(), GetMetaOnly::Yes);
                cb->callback(rv);
            };

    auto status = magma->Rollback(vbid.get(), rollbackSeqno, keyCallback);
    if (!status) {
        logger->critical("MagmaKVStore::rollback Rollback {} status:{}",
                         vbid,
                         status.String());
        return RollbackResult(false);
    }

    // Did a rollback, so need to reload the vbstate cache.
    status = loadVBStateCache(vbid);
    if (!status.IsOK()) {
        logger->error("MagmaKVStore::rollback {} readVBStateFromDisk status:{}",
                      vbid,
                      status.String());
        return RollbackResult(false);
    }

    auto vbstate = getVBucketState(vbid);
    if (!vbstate) {
        logger->critical(
                "MagmaKVStore::rollback getVBState {} vbstate not found", vbid);
        return RollbackResult(false);
    }

    return RollbackResult(true,
                          vbstate->highSeqno,
                          vbstate->lastSnapStart,
                          vbstate->lastSnapEnd);
}

Collections::KVStore::Manifest MagmaKVStore::getCollectionsManifest(Vbid vbid) {
    Status status;

    std::string manifest;
    std::tie(status, manifest) = readLocalDoc(vbid, manifestKey);

    std::string openCollections;
    std::tie(status, openCollections) = readLocalDoc(vbid, openCollectionsKey);

    std::string openScopes;
    std::tie(status, openScopes) = readLocalDoc(vbid, openScopesKey);

    std::string droppedCollections;
    std::tie(status, droppedCollections) = readLocalDoc(vbid, droppedCollectionsKey);
    return Collections::KVStore::decodeManifest(
            {reinterpret_cast<const uint8_t*>(manifest.data()),
             manifest.length()},
            {reinterpret_cast<const uint8_t*>(openCollections.data()),
             openCollections.length()},
            {reinterpret_cast<const uint8_t*>(openScopes.data()),
             openScopes.length()},
            {reinterpret_cast<const uint8_t*>(droppedCollections.data()),
             droppedCollections.length()});
}

std::vector<Collections::KVStore::DroppedCollection>
MagmaKVStore::getDroppedCollections(Vbid vbid) {
    Status status;
    std::string dropped;
    std::tie(status, dropped) = readLocalDoc(vbid, droppedCollectionsKey);
    return Collections::KVStore::decodeDroppedCollections(
            {reinterpret_cast<const uint8_t*>(dropped.data()),
             dropped.length()});
}

Status MagmaKVStore::updateCollectionsMeta(
        Vbid vbid,
        Magma::CommitBatch& commitBatch,
        Collections::VB::Flush& collectionsFlush) {
    auto status = updateManifestUid(commitBatch);
    if (!status) {
        return status;
    }

    // If the updateOpenCollections reads the dropped collections, it can pass
    // them via this optional to updateDroppedCollections, thus we only read
    // the dropped list once per update.
    std::optional<std::vector<Collections::KVStore::DroppedCollection>> dropped;

    if (!collectionsMeta.collections.empty() ||
        !collectionsMeta.droppedCollections.empty()) {
        std::tie(status, dropped) = updateOpenCollections(vbid, commitBatch);
        if (!status) {
            return status;
        }
    }

    if (!collectionsMeta.droppedCollections.empty()) {
        if (!dropped.has_value()) {
            dropped = getDroppedCollections(vbid);
        }
        status = updateDroppedCollections(vbid, commitBatch, dropped);
        if (!status) {
            return status;
        }
        collectionsFlush.setNeedsPurge();
    }

    if (!collectionsMeta.scopes.empty() ||
        !collectionsMeta.droppedScopes.empty()) {
        status = updateScopes(vbid, commitBatch);
        if (!status) {
            return status;
        }
    }

    collectionsMeta.clear();
    return Status::OK();
}

Status MagmaKVStore::updateManifestUid(Magma::CommitBatch& commitBatch) {
    // write back, no read required
    auto buf = Collections::KVStore::encodeManifestUid(collectionsMeta);
    Slice valSlice = {reinterpret_cast<const char*>(buf.data()), buf.size()};
    return setLocalDoc(commitBatch, manifestKey, valSlice);
}

std::pair<magma::Status, std::vector<Collections::KVStore::DroppedCollection>>
MagmaKVStore::updateOpenCollections(Vbid vbid,
                                    Magma::CommitBatch& commitBatch) {
    auto dropped = getDroppedCollections(vbid);
    Status status;
    std::string collections;
    std::tie(status, collections) = readLocalDoc(vbid, openCollectionsKey);
    auto buf = Collections::KVStore::encodeOpenCollections(
            dropped,
            collectionsMeta,
            {reinterpret_cast<const uint8_t*>(collections.data()),
             collections.length()});

    Slice valSlice = {reinterpret_cast<const char*>(buf.data()), buf.size()};
    status = setLocalDoc(commitBatch, openCollectionsKey, valSlice);
    return std::make_pair(status, dropped);
}

magma::Status MagmaKVStore::updateDroppedCollections(
        Vbid vbid,
        Magma::CommitBatch& commitBatch,
        std::optional<std::vector<Collections::KVStore::DroppedCollection>>
                dropped) {
    for (const auto& drop : collectionsMeta.droppedCollections) {
        // Delete the 'stats' document for the collection
        deleteCollectionStats(commitBatch, drop.collectionId);
    }

    // If the input 'dropped' is not initialised we must read the dropped
    // collection data
    if (!dropped.has_value()) {
        dropped = getDroppedCollections(vbid);
    }

    auto buf = Collections::KVStore::encodeDroppedCollections(collectionsMeta,
                                                              dropped.value());
    Slice valSlice = {reinterpret_cast<const char*>(buf.data()), buf.size()};
    return setLocalDoc(commitBatch, droppedCollectionsKey, valSlice);
}

std::string MagmaKVStore::getCollectionsStatsKey(CollectionID cid) {
    return std::string{"|" + cid.to_string() + "|"};
}

void MagmaKVStore::saveCollectionStats(
        magma::Magma::CommitBatch& commitBatch,
        CollectionID cid,
        const Collections::VB::PersistedStats& stats) {
    auto key = getCollectionsStatsKey(cid);
    Slice keySlice(key);
    auto encodedStats = stats.getLebEncodedStats();
    Slice valSlice(encodedStats);
    auto status = setLocalDoc(commitBatch, keySlice, valSlice);
    if (!status) {
        logger->warn(
                "MagmaKVStore::saveCollectionStats setLocalDoc {} failed. "
                "status:{}",
                keySlice.ToString(),
                status.String());
    }
    return;
}

std::optional<Collections::VB::PersistedStats> MagmaKVStore::getCollectionStats(
        const KVFileHandle& kvFileHandle, CollectionID cid) {
    const auto& kvfh = static_cast<const MagmaKVFileHandle&>(kvFileHandle);
    auto vbid = kvfh.vbid;
    auto key = getCollectionsStatsKey(cid);
    Slice keySlice(key);
    Status status;
    std::string stats;
    std::tie(status, stats) = readLocalDoc(vbid, keySlice);
    if (!status) {
        return {};
    }
    return Collections::VB::PersistedStats(stats.c_str(), stats.size());
}

magma::Status MagmaKVStore::deleteCollectionStats(
        Magma::CommitBatch& commitBatch, CollectionID cid) {
    auto key = getCollectionsStatsKey(cid);
    Slice keySlice(key);
    return deleteLocalDoc(commitBatch, keySlice);
}

magma::Status MagmaKVStore::updateScopes(Vbid vbid,
                                         Magma::CommitBatch& commitBatch) {
    Status status;
    std::string scopes;
    std::tie(status, scopes) = readLocalDoc(vbid, openScopesKey);
    auto buf = encodeScopes(
            collectionsMeta,
            {reinterpret_cast<const uint8_t*>(scopes.data()), scopes.length()});
    Slice valSlice = {reinterpret_cast<const char*>(buf.data()), buf.size()};
    return setLocalDoc(commitBatch, openScopesKey, valSlice);
}

bool MagmaKVStore::getStat(const char* name, size_t& value) {
    Magma::MagmaStats magmaStats;
    if (strncmp(name, "memory_quota", sizeof("memory_quota")) == 0) {
        magma->GetStats(magmaStats);
        value = static_cast<size_t>(magmaStats.MemoryQuota);
    } else if (strncmp(name,
                       "write_cache_quota",
                       sizeof("write_cache_quota")) == 0) {
        magma->GetStats(magmaStats);
        value = static_cast<size_t>(magmaStats.WriteCacheQuota);
    } else if (strcmp("failure_get", name) == 0) {
        value = st.numGetFailure.load();
    } else if (strcmp("failure_compaction", name) == 0) {
        value = st.numCompactionFailure.load();
    } else {
        return false;
    }
    return true;
}

void MagmaKVStore::addStats(const AddStatFn& add_stat,
                            const void* c,
                            const std::string& args) {
    KVStore::addStats(add_stat, c, args);
    const auto prefix = getStatsPrefix();

    Magma::MagmaStats stats;
    magma->GetStats(stats);
    auto statName = prefix + ":magma";
    add_casted_stat(statName.c_str(), stats.JSON().dump(), add_stat, c);
}

MagmaInfo& MagmaKVStore::getMagmaInfo(Vbid vbid) {
    if (!cachedMagmaInfo[vbid.get()]) {
        auto minfo = std::make_unique<MagmaInfo>();
        cachedMagmaInfo[vbid.get()] = std::move(minfo);
    }
    return *cachedMagmaInfo[vbid.get()];
}

void MagmaKVStore::pendingTasks() {
    std::queue<std::tuple<Vbid, uint64_t>> vbucketsToDelete;
    vbucketsToDelete.swap(*pendingVbucketDeletions.wlock());
    while (!vbucketsToDelete.empty()) {
        Vbid vbid;
        uint64_t vb_version;
        std::tie(vbid, vb_version) = vbucketsToDelete.front();
        vbucketsToDelete.pop();
        delVBucket(vbid, vb_version);
    }
}

compaction_ctx MagmaKVStore::makeCompactionContext(Vbid vbid) {
    if (!makeCompactionContextCallback) {
        throw std::runtime_error(
                "MagmaKVStore::makeCompactionContext: Have not set "
                "makeCompactionContextCallback to create a compaction_ctx");
    }

    CompactionConfig config{};
    config.db_file_id = vbid;
    auto ctx = makeCompactionContextCallback(config, 0 /*purgeSeqno*/);

    ctx.eraserContext = std::make_unique<Collections::VB::EraserContext>(
            getDroppedCollections(vbid));

    auto readState = readVBStateFromDisk(vbid);
    if (!readState.status.IsOK()) {
        std::stringstream ss;
        ss << "MagmaKVStore::makeCompactionContext " << vbid
           << " failed to read vbstate from disk. Status:"
           << readState.status.String();
        throw std::runtime_error(ss.str());
    } else {
        ctx.highCompletedSeqno = readState.vbstate.persistedCompletedSeqno;
    }
    return ctx;
}

const KVStoreConfig& MagmaKVStore::getConfig() const {
    return configuration;
}

DBFileInfo MagmaKVStore::getAggrDbFileInfo() {
    Magma::MagmaStats stats;
    magma->GetStats(stats);
    DBFileInfo vbinfo(stats.ActiveDiskUsage, stats.ActiveDataSize);
    return vbinfo;
}

Status MagmaKVStore::writeVBStateToDisk(Vbid vbid,
                                        const vbucket_state& vbstate,
                                        const MagmaInfo& minfo) {
    std::unique_ptr<Magma::CommitBatch> batch;
    auto status = magma->NewCommitBatch(
            vbid.get(),
            batch,
            static_cast<Magma::KVStoreRevision>(minfo.kvstoreRev));

    if (!status) {
        logger->critical(
                "MagmaKVStore::writeVBStateToDisk failed creating "
                "commitBatch for "
                "{} status:{}",
                vbid,
                status.String());
        return status;
    }

    writeVBStateToDisk(vbid, *(batch.get()), vbstate, minfo);

    status = magma->ExecuteCommitBatch(std::move(batch));
    if (!status) {
        logger->critical(
                "MagmaKVStore::writeVBStateToDisk: "
                "magma::ExecuteCommitBatch "
                "{} status:{}",
                vbid,
                status.String());
        return status;
    }

    status = magma->SyncCommitBatches(commitPointEveryBatch);
    if (!status) {
        logger->critical(
                "MagmaKVStore::writeVBStateToDisk: "
                "magma::SyncCommitBatches {} "
                "status:{}",
                vbid,
                status.String());
        return status;
    }
    return Status::OK();
}
