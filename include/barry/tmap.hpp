#pragma once
#include <sys/mman.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace barry {

// The temporal address space: mmap for time.
//
// mmap turns a file range into an address range; Tmap turns a TIME range into
// an address range. A slot's location is pure arithmetic on its timestamp,
//
//     slot(T) = base + ((T - T0) >> S) * sizeof(Slot),
//
// over one anonymous reservation, and everything the engine is built on
// follows from that identity rather than being engineered beside it:
//
//   1. placement is a subtraction and a shift, O(1) worst case, with no
//      data-dependent memory access (Theorem: addressing);
//   2. residency is proportional to OCCUPIED time, not spanned time, because
//      untouched slots are never faulted in (Theorem: space);
//   3. a time prefix IS an address prefix, so retention is madvise(DONTNEED)
//      over [base, slot(t)) and nothing else is disturbed;
//   4. slot contents are whatever the client appends, so if the client's slot
//      state is a pure function of its appended multiset, replaying an
//      append log reconstructs the region exactly (recovery = re-ingestion).
//
// The graph engine's Timeline is one instantiation (Slot = block pointer).
// This header exists so a client that is NOT a graph can be built on the same
// contract -- tests/test_tmap.cpp does it with per-window atomic counters --
// which is the difference between an abstraction and a formula.
//
// Slot must be trivially default-constructible from zero bytes: fresh
// anonymous pages read as zeros, and "all zeroes" must mean "empty slot".
template <class Slot>
class Tmap {
public:
    Tmap(uint64_t t0, uint32_t shift, size_t slots)
        : t0_(t0), shift_(shift), slots_(slots) {
        bytes_ = slots_ * sizeof(Slot);
        void* p = ::mmap(nullptr, bytes_, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) { std::perror("[Tmap] mmap"); std::abort(); }
        base_ = static_cast<Slot*>(p);
    }
    ~Tmap() { if (base_) ::munmap(base_, bytes_); }

    Tmap(const Tmap&) = delete;
    Tmap& operator=(const Tmap&) = delete;

    // Clause 1: the address is the arithmetic. No bounds branch on the hot
    // path beyond the guard; no search structure exists to consult.
    Slot* slot_for(uint64_t t) noexcept {
        const uint64_t idx = (t - t0_) >> shift_;
        return idx < slots_ ? base_ + idx : nullptr;
    }
    const Slot* slot_for(uint64_t t) const noexcept {
        const uint64_t idx = (t - t0_) >> shift_;
        return idx < slots_ ? base_ + idx : nullptr;
    }

    // Clause 3: retention is an address-prefix release. Whole pages covering
    // slots strictly before t return to the OS; later slots are untouched and
    // concurrent readers of them never notice.
    void evict_before(uint64_t t) noexcept {
        const uint64_t idx = (t - t0_) >> shift_;
        const size_t upto = (idx < slots_ ? idx : slots_) * sizeof(Slot);
        const size_t page = 4096;
        const size_t whole = (upto / page) * page;
        if (whole) ::madvise(base_, whole, MADV_DONTNEED);
        evicted_before_ = t0_ + ((whole / sizeof(Slot)) << shift_);
    }
    uint64_t min_valid_time() const noexcept { return evicted_before_; }

    // Range iteration for clients; [t_lo, t_hi] inclusive, clamped.
    template <class F>
    void for_range(uint64_t t_lo, uint64_t t_hi, F&& f) const {
        uint64_t lo = t_lo < t0_ ? 0 : (t_lo - t0_) >> shift_;
        uint64_t hi = (t_hi - t0_) >> shift_;
        if (hi >= slots_) hi = slots_ - 1;
        for (uint64_t i = lo; i <= hi; ++i) f(i, base_[i]);
    }

    uint64_t t0()    const noexcept { return t0_; }
    uint32_t shift() const noexcept { return shift_; }
    size_t   slots() const noexcept { return slots_; }

private:
    uint64_t t0_;
    uint32_t shift_;
    size_t   slots_;
    size_t   bytes_ = 0;
    Slot*    base_ = nullptr;
    uint64_t evicted_before_ = 0;
};

}  // namespace barry
