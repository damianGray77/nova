#pragma once

// HashMap<V>: u64 keys to trivially copyable values. Open addressing with
// linear probing and backward-shift deletion (no tombstones), power-of-two
// capacity, load factor <= 0.7.
//
// Iteration (slot_*) visits slots in index order. Slot positions depend only on
// the sequence of inserts and erases, so iteration is deterministic too.
#include "nova/array.h"
#include "nova/hash.h"

namespace nova {

template <class V> class HashMap {
    static_assert(__is_trivially_copyable(V), "nova::HashMap values must be trivially copyable");

  public:
    usize size() const { return size_; }
    bool empty() const { return size_ == 0; }

    V* find(u64 key) {
        usize s = find_slot(key);
        return s == kNone ? nullptr : &vals_[s];
    }
    const V* find(u64 key) const {
        usize s = find_slot(key);
        return s == kNone ? nullptr : &vals_[s];
    }
    bool contains(u64 key) const { return find_slot(key) != kNone; }

    // Returns the value for key, inserting a zero-filled value if absent.
    // The reference is valid until the next insert (which may rehash).
    V& get_or_insert(u64 key, bool* inserted = nullptr) {
        if ((size_ + 1) * 10 > used_.size() * 7)
            grow();
        usize mask = used_.size() - 1;
        usize i = static_cast<usize>(mix64(key)) & mask;
        while (used_[i]) {
            if (keys_[i] == key) {
                if (inserted)
                    *inserted = false;
                return vals_[i];
            }
            i = (i + 1) & mask;
        }
        used_[i] = 1;
        keys_[i] = key;
        memset(static_cast<void*>(&vals_[i]), 0, sizeof(V));
        ++size_;
        if (inserted)
            *inserted = true;
        return vals_[i];
    }

    bool erase(u64 key) {
        usize i = find_slot(key);
        if (i == kNone)
            return false;
        usize mask = used_.size() - 1;
        used_[i] = 0;
        --size_;
        // Backward shift: pull later entries of the probe run into the hole if
        // their home slot is not cyclically inside (i, j].
        usize j = i;
        for (;;) {
            j = (j + 1) & mask;
            if (!used_[j])
                return true;
            usize home = static_cast<usize>(mix64(keys_[j])) & mask;
            bool stays = (i <= j) ? (i < home && home <= j) : (i < home || home <= j);
            if (stays)
                continue;
            keys_[i] = keys_[j];
            vals_[i] = vals_[j];
            used_[i] = 1;
            used_[j] = 0;
            i = j;
        }
    }

    void clear() {
        used_.fill(0);
        size_ = 0;
    }

    usize slot_count() const { return used_.size(); }
    bool slot_used(usize i) const { return used_[i] != 0; }
    u64 slot_key(usize i) const { return keys_[i]; }
    V& slot_value(usize i) { return vals_[i]; }
    const V& slot_value(usize i) const { return vals_[i]; }

  private:
    static constexpr usize kNone = ~static_cast<usize>(0);

    usize find_slot(u64 key) const {
        if (size_ == 0)
            return kNone;
        usize mask = used_.size() - 1;
        usize i = static_cast<usize>(mix64(key)) & mask;
        while (used_[i]) {
            if (keys_[i] == key)
                return i;
            i = (i + 1) & mask;
        }
        return kNone;
    }

    void grow() {
        usize cap = used_.size() < 16 ? 16 : used_.size() * 2;
        Array<u64> old_keys = nova::move(keys_);
        Array<V> old_vals = nova::move(vals_);
        Array<u8> old_used = nova::move(used_);
        keys_.resize(cap);
        vals_.resize(cap);
        used_.resize(cap);
        size_ = 0;
        for (usize s = 0; s < old_used.size(); ++s)
            if (old_used[s])
                get_or_insert(old_keys[s]) = old_vals[s];
    }

    Array<u64> keys_;
    Array<V> vals_;
    Array<u8> used_;
    usize size_ = 0;
};

} // namespace nova
