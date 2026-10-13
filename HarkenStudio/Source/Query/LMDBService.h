
#pragma once

#ifdef HARKEN_HAS_LMDB
#include <lmdb.h>
#endif

#include <QByteArray>
#include <QString>
#include <QVector>
#include <memory>
#include <cstdint>

namespace VNX {

class LMDBService {
public:
    static std::unique_ptr<LMDBService> Create();
    ~LMDBService();

    bool open(const QString& dbPath, size_t mapSizeMB = 64);
    void close();
    bool isOpen() const;

    enum DBI : int {
        Object = 0,
        Inverted = 1,
        Trigram = 2,
        Type = 3,
        DBI_COUNT = 4
    };

    class ReadTxn {
    public:
        ReadTxn() = default;
#ifdef HARKEN_HAS_LMDB
        explicit ReadTxn(MDB_txn* txn, MDB_dbi* dbis);
#endif
        ~ReadTxn();
        ReadTxn(ReadTxn&& o) noexcept;
        ReadTxn& operator=(ReadTxn&& o) noexcept;
        ReadTxn(const ReadTxn&) = delete;
        ReadTxn& operator=(const ReadTxn&) = delete;

        bool valid() const;
        int  get(DBI dbi, const void* keyData, size_t keySize,
                 void** outData, size_t* outSize);
        void abort();

    private:
#ifdef HARKEN_HAS_LMDB
        MDB_txn* txn_ = nullptr;
        MDB_dbi  dbis_[DBI_COUNT] = {};
#endif
    };

    class WriteTxn {
    public:
        WriteTxn() = default;
#ifdef HARKEN_HAS_LMDB
        explicit WriteTxn(MDB_txn* txn, MDB_dbi* dbis);
#endif
        ~WriteTxn();
        WriteTxn(WriteTxn&& o) noexcept;
        WriteTxn& operator=(WriteTxn&& o) noexcept;
        WriteTxn(const WriteTxn&) = delete;
        WriteTxn& operator=(const WriteTxn&) = delete;

        bool valid() const;
        int  get(DBI dbi, const void* keyData, size_t keySize,
                 void** outData, size_t* outSize);
        int  put(DBI dbi, const void* keyData, size_t keySize,
                 const void* valData, size_t valSize, unsigned int flags = 0);
        int  del(DBI dbi, const void* keyData, size_t keySize);
        int  clear(DBI dbi);
        bool allKeys(DBI dbi, QVector<QByteArray>& out);
        bool commit();
        void abort();

    private:
#ifdef HARKEN_HAS_LMDB
        MDB_txn* txn_ = nullptr;
        MDB_dbi  dbis_[DBI_COUNT] = {};
#endif
    };

    ReadTxn  beginRead();
    WriteTxn beginWrite();

private:
    LMDBService() = default;
#ifdef HARKEN_HAS_LMDB
    MDB_env* env_ = nullptr;
    MDB_dbi  dbis_[DBI_COUNT] = {};
    bool     dbisOpened_ = false;
#endif
    bool opened_ = false;
};

}
