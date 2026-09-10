#include "barry/wal.hpp"
#include "barry/engine.hpp"

#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/uio.h>
#include <unistd.h>
#include <vector>
#include <zlib.h>

namespace barry {

namespace {
constexpr uint32_t kMagic = 0x4C415742u;  // 'BWAL'
struct FrameHeader { uint32_t magic, count, crc; };
}  // namespace

Wal::Wal(const std::string& path, size_t sync_every_bytes)
    : sync_every_(sync_every_bytes) {
    fd_ = ::open(path.c_str(), O_CREAT | O_WRONLY | O_APPEND, 0644);
    if (fd_ < 0) std::perror(path.c_str());
}

Wal::~Wal() {
    if (fd_ >= 0) { ::fsync(fd_); ::close(fd_); }
}

bool Wal::append(const Edge* edges, size_t n) {
    if (fd_ < 0 || n == 0) return fd_ >= 0;
    FrameHeader h{kMagic, static_cast<uint32_t>(n),
                  static_cast<uint32_t>(::crc32(0,
                      reinterpret_cast<const Bytef*>(edges),
                      static_cast<uInt>(n * sizeof(Edge))))};
    struct iovec iov[2] = {
        {&h, sizeof(h)},
        {const_cast<Edge*>(edges), n * sizeof(Edge)},
    };
    ssize_t want = static_cast<ssize_t>(sizeof(h) + n * sizeof(Edge));
    if (::writev(fd_, iov, 2) != want) { std::perror("[Wal] writev"); return false; }
    bytes_ += static_cast<size_t>(want);
    unsynced_ += static_cast<size_t>(want);
    if (unsynced_ >= sync_every_) return sync();
    return true;
}

bool Wal::sync() {
    if (fd_ < 0) return false;
    if (::fsync(fd_) != 0) { std::perror("[Wal] fsync"); return false; }
    unsynced_ = 0;
    return true;
}

Wal::ReplayStats Wal::replay(const std::string& path, Barry& into, int num_threads) {
    ReplayStats st;
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return st;

    std::vector<Edge> batch;
    FrameHeader h;
    for (;;) {
        long frame_start = std::ftell(f);
        if (std::fread(&h, sizeof(h), 1, f) != 1) break;               // clean EOF
        if (h.magic != kMagic) {
            std::fseek(f, 0, SEEK_END);
            st.torn_bytes = static_cast<size_t>(std::ftell(f) - frame_start);
            break;                                                      // garbage header
        }
        batch.resize(h.count);
        if (std::fread(batch.data(), sizeof(Edge), h.count, f) != h.count) {
            std::fseek(f, 0, SEEK_END);
            st.torn_bytes = static_cast<size_t>(std::ftell(f) - frame_start);
            break;                                                      // torn payload
        }
        uint32_t crc = static_cast<uint32_t>(::crc32(0,
            reinterpret_cast<const Bytef*>(batch.data()),
            static_cast<uInt>(batch.size() * sizeof(Edge))));
        if (crc != h.crc) {
            std::fseek(f, 0, SEEK_END);
            st.torn_bytes = static_cast<size_t>(std::ftell(f) - frame_start);
            break;                                                      // corrupt frame
        }
        into.insert_batch(batch.data(), batch.size(), num_threads);
        st.edges += batch.size();
        ++st.frames;
    }
    std::fclose(f);
    return st;
}

AsyncWal::AsyncWal(const std::string& path, size_t sync_every_bytes)
    : inner_(path, sync_every_bytes) {
    if (inner_.ok()) writer_ = std::thread(&AsyncWal::writer_loop, this);
}

AsyncWal::~AsyncWal() {
    {
        std::lock_guard<std::mutex> g(mu_);
        stop_ = true;
    }
    cv_writer_.notify_one();
    if (writer_.joinable()) writer_.join();
}

void AsyncWal::append(const Edge* edges, size_t n) {
    if (!inner_.ok() || n == 0) return;
    std::vector<Edge> copy(edges, edges + n);
    std::unique_lock<std::mutex> lk(mu_);
    // Backpressure: an unbounded queue would hide a disk that cannot keep up and
    // then lose everything queued on a crash. Bounding it makes the overload
    // case visible as throughput instead of as silent risk.
    cv_producer_.wait(lk, [&] { return queued_bytes_ < kMaxQueuedBytes || io_error_; });
    if (io_error_) return;
    queued_bytes_ += copy.size() * sizeof(Edge);
    q_.push_back(std::move(copy));
    lk.unlock();
    cv_writer_.notify_one();
}

bool AsyncWal::sync() {
    std::unique_lock<std::mutex> lk(mu_);
    cv_producer_.wait(lk, [&] { return q_.empty() || io_error_; });
    if (io_error_) return false;
    lk.unlock();
    return inner_.sync();
}

void AsyncWal::writer_loop() {
    for (;;) {
        std::unique_lock<std::mutex> lk(mu_);
        cv_writer_.wait(lk, [&] { return stop_ || !q_.empty(); });
        if (q_.empty() && stop_) break;
        std::vector<Edge> batch = std::move(q_.front());
        q_.pop_front();
        queued_bytes_ -= batch.size() * sizeof(Edge);
        lk.unlock();
        cv_producer_.notify_all();
        if (!inner_.append(batch.data(), batch.size())) {
            std::lock_guard<std::mutex> g(mu_);
            io_error_ = true;
            cv_producer_.notify_all();
        }
    }
    inner_.sync();
}

}  // namespace barry
