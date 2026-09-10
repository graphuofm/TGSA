#pragma once
#include "edge.hpp"
#include <condition_variable>
#include <cstdint>
#include <cstddef>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace barry {

class Barry;

// Write-ahead log for the ingest path.
//
// Barry's durability story is simpler than a general store's because the engine
// state is a pure function of the edge multiset: edges are append-only, never
// updated, never semantically deleted. Recovery therefore IS re-ingestion --
// replaying the log through the normal insert path reconstructs an equivalent
// engine with no undo, no redo phases, and no idempotence bookkeeping. What the
// log must guarantee is only that every acknowledged batch is readable and
// detectably complete, which a length + CRC frame provides.
//
// Frame layout (little-endian):
//   magic   u32  'BWAL'
//   count   u32  edges in this batch
//   crc32   u32  over the edge payload
//   payload count * sizeof(Edge)
//
// A torn final frame (crash mid-write) fails its length or CRC check and is
// discarded; every earlier frame was fsync'd before append() returned (fsync
// policy: every `sync_every_bytes`, default 4 MB, plus on close).
class Wal {
public:
    explicit Wal(const std::string& path, size_t sync_every_bytes = 4u << 20);
    ~Wal();

    Wal(const Wal&) = delete;
    Wal& operator=(const Wal&) = delete;

    bool ok() const noexcept { return fd_ >= 0; }

    // Append one batch; returns false on I/O error. Durable up to the last
    // sync boundary once this returns.
    bool append(const Edge* edges, size_t n);

    // Force everything appended so far to stable storage.
    bool sync();

    size_t bytes_written() const noexcept { return bytes_; }

    // Replay a log into an engine via the normal insert path. Returns the
    // number of edges recovered; stops (without error) at the first torn or
    // corrupt frame, which is the crash-consistency contract.
    struct ReplayStats {
        size_t edges = 0;
        size_t frames = 0;
        size_t torn_bytes = 0;   // trailing bytes discarded
    };
    static ReplayStats replay(const std::string& path, Barry& into,
                              int num_threads = 0);

private:
    int    fd_ = -1;
    size_t bytes_ = 0;
    size_t unsynced_ = 0;
    size_t sync_every_;
};

// Asynchronous front-end: append() copies the batch onto a queue and returns;
// a writer thread drains the queue through the underlying Wal. Ingestion no
// longer waits on the disk -- the durability point moves from "append returned"
// to "sync() returned", which is the contract a streaming collector actually
// wants (ack after group commit, ingest at memory speed).
class AsyncWal {
public:
    explicit AsyncWal(const std::string& path, size_t sync_every_bytes = 4u << 20);
    ~AsyncWal();

    AsyncWal(const AsyncWal&) = delete;
    AsyncWal& operator=(const AsyncWal&) = delete;

    bool ok() const noexcept { return inner_.ok(); }

    // Copies the batch and returns immediately. Durable only after sync().
    void append(const Edge* edges, size_t n);

    // Drain the queue, then fsync. On return every prior append is durable.
    bool sync();

private:
    void writer_loop();

    Wal inner_;
    std::thread writer_;
    std::mutex mu_;
    std::condition_variable cv_producer_, cv_writer_;
    std::deque<std::vector<Edge>> q_;
    size_t queued_bytes_ = 0;
    bool stop_ = false;
    bool io_error_ = false;
    static constexpr size_t kMaxQueuedBytes = 64u << 20;   // backpressure bound
};

}  // namespace barry
