#ifndef EXCHANGE_POSITION_TRAVERSAL_ORDER_HPP
#define EXCHANGE_POSITION_TRAVERSAL_ORDER_HPP
#include <algorithm>
#include <bit>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace exchange {
// Bucket probing and traversal adapted from Eclipse Collections 11.0.0
// (Apache-2.0).
class PositionTraversalOrder {
public:
    void insert(std::uint32_t key) {
        if (key == 0) {
            throw std::invalid_argument{"zero symbol"};
        }
        if (key == 1) {
            one_ = true;
            return;
        }
        const auto at = probe(key);
        if (slots_[at] == key) {
            return;
        }
        if (slots_[at] == 1) {
            --removed_;
        }
        slots_[at] = key;
        ++size_;
        const auto max = slots_.size() / 2;
        if (size_ + removed_ > max) {
            auto capacity = std::max(max, std::bit_ceil((size_ + 1) * 2));
            if (removed_ && size_ > (max >> 1) + (max >> 2)) {
                capacity *= 2;
            }
            auto old = std::move(slots_);
            slots_.assign(capacity, 0);
            size_ = removed_ = 0;
            for (const auto k : old) {
                if (k > 1) {
                    insert(k);
                }
            }
        }
    }
    void erase(std::uint32_t key) {
        if (key == 1) {
            one_ = false;
            return;
        }
        const auto at = probe(key);
        if (slots_[at] == key) {
            slots_[at] = 1;
            --size_;
            ++removed_;
        }
    }
    std::vector<std::uint32_t> keys() const {
        std::vector<std::uint32_t> result;
        if (one_) {
            result.push_back(1);
        }
        for (const auto key : slots_) {
            if (key > 1) {
                result.push_back(key);
            }
        }
        return result;
    }
    const std::vector<std::uint32_t>& slots() const {
        return slots_;
    }
    bool one() const {
        return one_;
    }
    static PositionTraversalOrder restore(std::vector<std::uint32_t> slots,
                                          bool one) {
        if (slots.size() < 16 || !std::has_single_bit(slots.size())) {
            throw std::runtime_error{"position table size"};
        }
        PositionTraversalOrder out;
        out.slots_ = std::move(slots);
        out.one_ = one;
        for (const auto k : out.slots_) {
            out.size_ += k > 1;
            out.removed_ += k == 1;
        }
        if (out.size_ + out.removed_ > out.slots_.size() / 2) {
            throw std::runtime_error{"position table load"};
        }
        for (std::size_t i = 0; i < out.slots_.size(); ++i) {
            if (out.slots_[i] > 1 && out.probe(out.slots_[i]) != i) {
                throw std::runtime_error{"position table probe or duplicate"};
            }
        }
        return out;
    }
    bool operator==(const PositionTraversalOrder&) const = default;

private:
    static std::uint32_t spread1(std::uint32_t k) {
        k ^= k >> 15;
        k *= std::uint32_t(-1398068659);
        k ^= k >> 15;
        k *= 1556602707u;
        return k ^ (k >> 12);
    }
    static std::uint32_t spread2(std::uint32_t k) {
        k ^= k >> 14;
        k *= std::uint32_t(-1172517581);
        k ^= k >> 13;
        k *= std::uint32_t(-1688037685);
        return k ^ (k >> 12);
    }
    static std::uint32_t reverse(std::uint32_t v) {
        std::uint32_t r{};
        for (unsigned i = 0; i < 32; ++i) {
            r = (r << 1) | (v & 1);
            v >>= 1;
        }
        return r;
    }
    std::size_t probe(std::uint32_t key) const {
        const auto mask = slots_.size() - 1;
        std::size_t removed = std::numeric_limits<std::size_t>::max();
        const auto check = [&](std::size_t at) -> std::size_t {
            const auto value = slots_[at];
            if (value == key) {
                return at;
            }
            if (value == 0) {
                return removed == std::numeric_limits<std::size_t>::max()
                           ? at
                           : removed;
            }
            if (value == 1 &&
                removed == std::numeric_limits<std::size_t>::max()) {
                removed = at;
            }
            return std::numeric_limits<std::size_t>::max();
        };
        for (unsigned stage = 0; stage < 2; ++stage) {
            const auto start = stage == 0 ? key : spread2(key);
            for (unsigned i = 0; i < 8; ++i) {
                const auto at = check((start + i) & mask);
                if (at != std::numeric_limits<std::size_t>::max()) {
                    return at;
                }
            }
        }
        auto at = spread1(key);
        const auto step = reverse(spread2(key)) | 1u;
        for (std::size_t i = 0; i < slots_.size(); ++i) {
            at = (at + step) & static_cast<std::uint32_t>(mask);
            const auto found = check(at);
            if (found != std::numeric_limits<std::size_t>::max()) {
                return found;
            }
        }
        throw std::logic_error{"position probe did not terminate"};
    }
    std::vector<std::uint32_t> slots_ = std::vector<std::uint32_t>(16);
    std::size_t size_{}, removed_{};
    bool one_{};
};
}  // namespace exchange
#endif
