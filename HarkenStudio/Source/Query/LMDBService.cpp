
#include "LMDBService.h"
#include <QDebug>
#include <cstdlib>
#include <cstring>

namespace VNX {

static const char* kDBINames[LMDBService::DBI_COUNT] = {
    "object",
    "inverted",
    "trigram",
    "type"
};


std::unique_ptr<LMDBService> LMDBService::Create()
{
#ifndef HARKEN_HAS_LMDB
    qDebug() << "[LMDBService] Compiled without LMDB support";
    return nullptr;
#else
    if (const char* env = std::getenv("HARKEN_DISABLE_LMDB")) {
        if (std::strcmp(env, "1") == 0) {
            qDebug() << "[LMDBService] Disabled by HARKEN_DISABLE_LMDB=1";
            return nullptr;
        }
    }
    return std::unique_ptr<LMDBService>(new LMDBService());
#endif
}


LMDBService::~LMDBService()
{
    close();
}

bool LMDBService::open(const QString& dbPath, size_t mapSizeMB)
{
#ifndef HARKEN_HAS_LMDB
    Q_UNUSED(dbPath);
    Q_UNUSED(mapSizeMB);
    return false;
#else
    if (opened_) close();

    int rc = mdb_env_create(&env_);
    if (rc != 0) {
        qWarning() << "[LMDBService] mdb_env_create failed:" << mdb_strerror(rc);
        return false;
    }

    rc = mdb_env_set_mapsize(env_, mapSizeMB * 1024ULL * 1024ULL);
    if (rc != 0) {
        qWarning() << "[LMDBService] mdb_env_set_mapsize failed:" << mdb_strerror(rc);
        mdb_env_close(env_);
        env_ = nullptr;
        return false;
    }

    rc = mdb_env_set_maxdbs(env_, DBI_COUNT);
    if (rc != 0) {
        qWarning() << "[LMDBService] mdb_env_set_maxdbs failed:" << mdb_strerror(rc);
        mdb_env_close(env_);
        env_ = nullptr;
        return false;
    }

    QByteArray pathUtf8 = dbPath.toUtf8();
    rc = mdb_env_open(env_, pathUtf8.constData(), 0, 0664);
    if (rc != 0) {
        qWarning() << "[LMDBService] mdb_env_open failed:" << mdb_strerror(rc)
                    << "path:" << dbPath;
        mdb_env_close(env_);
        env_ = nullptr;
        return false;
    }

    MDB_txn* txn = nullptr;
    rc = mdb_txn_begin(env_, nullptr, 0, &txn);
    if (rc != 0) {
        qWarning() << "[LMDBService] mdb_txn_begin (open DBIs) failed:" << mdb_strerror(rc);
        mdb_env_close(env_);
        env_ = nullptr;
        return false;
    }

    for (int i = 0; i < DBI_COUNT; ++i) {
        rc = mdb_dbi_open(txn, kDBINames[i], MDB_CREATE, &dbis_[i]);
        if (rc != 0) {
            qWarning() << "[LMDBService] mdb_dbi_open failed for"
                        << kDBINames[i] << ":" << mdb_strerror(rc);
            mdb_txn_abort(txn);
            mdb_env_close(env_);
            env_ = nullptr;
            return false;
        }
    }

    rc = mdb_txn_commit(txn);
    if (rc != 0) {
        qWarning() << "[LMDBService] mdb_txn_commit (open DBIs) failed:" << mdb_strerror(rc);
        mdb_env_close(env_);
        env_ = nullptr;
        return false;
    }

    dbisOpened_ = true;
    opened_ = true;
    qDebug() << "[LMDBService] Opened" << dbPath << "mapSize=" << mapSizeMB << "MB";
    return true;
#endif
}

void LMDBService::close()
{
#ifdef HARKEN_HAS_LMDB
    if (env_) {
        if (dbisOpened_) {
            for (int i = 0; i < DBI_COUNT; ++i)
                mdb_dbi_close(env_, dbis_[i]);
            dbisOpened_ = false;
        }
        mdb_env_close(env_);
        env_ = nullptr;
    }
#endif
    opened_ = false;
}

bool LMDBService::isOpen() const
{
    return opened_;
}


LMDBService::ReadTxn LMDBService::beginRead()
{
#ifndef HARKEN_HAS_LMDB
    return ReadTxn();
#else
    if (!opened_ || !env_) return ReadTxn();

    MDB_txn* txn = nullptr;
    int rc = mdb_txn_begin(env_, nullptr, MDB_RDONLY, &txn);
    if (rc != 0) {
        qWarning() << "[LMDBService] beginRead failed:" << mdb_strerror(rc);
        return ReadTxn();
    }
    return ReadTxn(txn, dbis_);
#endif
}

LMDBService::WriteTxn LMDBService::beginWrite()
{
#ifndef HARKEN_HAS_LMDB
    return WriteTxn();
#else
    if (!opened_ || !env_) return WriteTxn();

    MDB_txn* txn = nullptr;
    int rc = mdb_txn_begin(env_, nullptr, 0, &txn);
    if (rc != 0) {
        qWarning() << "[LMDBService] beginWrite failed:" << mdb_strerror(rc);
        return WriteTxn();
    }
    return WriteTxn(txn, dbis_);
#endif
}


#ifdef HARKEN_HAS_LMDB
LMDBService::ReadTxn::ReadTxn(MDB_txn* txn, MDB_dbi* dbis)
    : txn_(txn)
{
    std::memcpy(dbis_, dbis, sizeof(MDB_dbi) * DBI_COUNT);
}
#endif

LMDBService::ReadTxn::~ReadTxn()
{
    abort();
}

LMDBService::ReadTxn::ReadTxn(ReadTxn&& o) noexcept
#ifdef HARKEN_HAS_LMDB
    : txn_(o.txn_)
{
    std::memcpy(dbis_, o.dbis_, sizeof(dbis_));
    o.txn_ = nullptr;
}
#else
{
    Q_UNUSED(o);
}
#endif

LMDBService::ReadTxn& LMDBService::ReadTxn::operator=(ReadTxn&& o) noexcept
{
    if (this != &o) {
        abort();
#ifdef HARKEN_HAS_LMDB
        txn_ = o.txn_;
        std::memcpy(dbis_, o.dbis_, sizeof(dbis_));
        o.txn_ = nullptr;
#endif
    }
    return *this;
}

bool LMDBService::ReadTxn::valid() const
{
#ifdef HARKEN_HAS_LMDB
    return txn_ != nullptr;
#else
    return false;
#endif
}

int LMDBService::ReadTxn::get(DBI dbi, const void* keyData, size_t keySize,
                              void** outData, size_t* outSize)
{
#ifndef HARKEN_HAS_LMDB
    Q_UNUSED(dbi); Q_UNUSED(keyData); Q_UNUSED(keySize);
    Q_UNUSED(outData); Q_UNUSED(outSize);
    return -1;
#else
    if (!txn_ || dbi < 0 || dbi >= DBI_COUNT) return -1;

    MDB_val key{ keySize, const_cast<void*>(keyData) };
    MDB_val val{};
    int rc = mdb_get(txn_, dbis_[dbi], &key, &val);
    if (rc == 0) {
        if (outData) *outData = val.mv_data;
        if (outSize) *outSize = val.mv_size;
    }
    return rc;
#endif
}

void LMDBService::ReadTxn::abort()
{
#ifdef HARKEN_HAS_LMDB
    if (txn_) {
        mdb_txn_abort(txn_);
        txn_ = nullptr;
    }
#endif
}


#ifdef HARKEN_HAS_LMDB
LMDBService::WriteTxn::WriteTxn(MDB_txn* txn, MDB_dbi* dbis)
    : txn_(txn)
{
    std::memcpy(dbis_, dbis, sizeof(MDB_dbi) * DBI_COUNT);
}
#endif

LMDBService::WriteTxn::~WriteTxn()
{
    abort();
}

LMDBService::WriteTxn::WriteTxn(WriteTxn&& o) noexcept
#ifdef HARKEN_HAS_LMDB
    : txn_(o.txn_)
{
    std::memcpy(dbis_, o.dbis_, sizeof(dbis_));
    o.txn_ = nullptr;
}
#else
{
    Q_UNUSED(o);
}
#endif

LMDBService::WriteTxn& LMDBService::WriteTxn::operator=(WriteTxn&& o) noexcept
{
    if (this != &o) {
        abort();
#ifdef HARKEN_HAS_LMDB
        txn_ = o.txn_;
        std::memcpy(dbis_, o.dbis_, sizeof(dbis_));
        o.txn_ = nullptr;
#endif
    }
    return *this;
}

bool LMDBService::WriteTxn::valid() const
{
#ifdef HARKEN_HAS_LMDB
    return txn_ != nullptr;
#else
    return false;
#endif
}

int LMDBService::WriteTxn::get(DBI dbi, const void* keyData, size_t keySize,
                               void** outData, size_t* outSize)
{
#ifndef HARKEN_HAS_LMDB
    Q_UNUSED(dbi); Q_UNUSED(keyData); Q_UNUSED(keySize);
    Q_UNUSED(outData); Q_UNUSED(outSize);
    return -1;
#else
    if (!txn_ || dbi < 0 || dbi >= DBI_COUNT) return -1;

    MDB_val key{ keySize, const_cast<void*>(keyData) };
    MDB_val val{};
    int rc = mdb_get(txn_, dbis_[dbi], &key, &val);
    if (rc == 0) {
        if (outData) *outData = val.mv_data;
        if (outSize) *outSize = val.mv_size;
    }
    return rc;
#endif
}

int LMDBService::WriteTxn::put(DBI dbi, const void* keyData, size_t keySize,
                               const void* valData, size_t valSize, unsigned int flags)
{
#ifndef HARKEN_HAS_LMDB
    Q_UNUSED(dbi); Q_UNUSED(keyData); Q_UNUSED(keySize);
    Q_UNUSED(valData); Q_UNUSED(valSize); Q_UNUSED(flags);
    return -1;
#else
    if (!txn_ || dbi < 0 || dbi >= DBI_COUNT) return -1;

    MDB_val key{ keySize, const_cast<void*>(keyData) };
    MDB_val val{ valSize, const_cast<void*>(valData) };
    return mdb_put(txn_, dbis_[dbi], &key, &val, flags);
#endif
}

int LMDBService::WriteTxn::del(DBI dbi, const void* keyData, size_t keySize)
{
#ifndef HARKEN_HAS_LMDB
    Q_UNUSED(dbi); Q_UNUSED(keyData); Q_UNUSED(keySize);
    return -1;
#else
    if (!txn_ || dbi < 0 || dbi >= DBI_COUNT) return -1;

    MDB_val key{ keySize, const_cast<void*>(keyData) };
    return mdb_del(txn_, dbis_[dbi], &key, nullptr);
#endif
}

int LMDBService::WriteTxn::clear(DBI dbi)
{
#ifndef HARKEN_HAS_LMDB
    Q_UNUSED(dbi);
    return -1;
#else
    if (!txn_ || dbi < 0 || dbi >= DBI_COUNT) return -1;

    return mdb_drop(txn_, dbis_[dbi], 0);
#endif
}

bool LMDBService::WriteTxn::allKeys(DBI dbi, QVector<QByteArray>& out)
{
#ifndef HARKEN_HAS_LMDB
    Q_UNUSED(dbi); Q_UNUSED(out);
    return false;
#else
    if (!txn_ || dbi < 0 || dbi >= DBI_COUNT) return false;

    MDB_cursor* cursor = nullptr;
    int rc = mdb_cursor_open(txn_, dbis_[dbi], &cursor);
    if (rc != 0) {
        qWarning() << "[LMDBService] allKeys mdb_cursor_open failed:" << mdb_strerror(rc);
        return false;
    }

    MDB_val key{};
    MDB_val val{};
    rc = mdb_cursor_get(cursor, &key, &val, MDB_FIRST);
    while (rc == 0) {
        out.push_back(QByteArray(static_cast<const char*>(key.mv_data),
                                 static_cast<int>(key.mv_size)));
        rc = mdb_cursor_get(cursor, &key, &val, MDB_NEXT);
    }
    mdb_cursor_close(cursor);

    return rc == MDB_NOTFOUND;
#endif
}

bool LMDBService::WriteTxn::commit()
{
#ifndef HARKEN_HAS_LMDB
    return false;
#else
    if (!txn_) return false;

    int rc = mdb_txn_commit(txn_);
    txn_ = nullptr;
    if (rc != 0) {
        qWarning() << "[LMDBService] WriteTxn commit failed:" << mdb_strerror(rc);
        return false;
    }
    return true;
#endif
}

void LMDBService::WriteTxn::abort()
{
#ifdef HARKEN_HAS_LMDB
    if (txn_) {
        mdb_txn_abort(txn_);
        txn_ = nullptr;
    }
#endif
}

}
