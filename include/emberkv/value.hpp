#pragma once

#include <cstddef>
#include <deque>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace emberkv {

/// Helper for std::visit with a set of lambdas.
template <class... Ts>
struct overloaded : Ts... {
    using Ts::operator()...;
};

using StringValue = std::string;

/// A list of strings that keeps a running byte count, so weighing a list for
/// the memory budget is O(1) instead of a walk over every element.
class ListValue {
public:
    using container = std::deque<std::string>;  // O(1) push/pop at both ends
    using const_iterator = container::const_iterator;

    void push_front(std::string s) {
        bytes_ += cost(s);
        items_.push_front(std::move(s));
    }
    void push_back(std::string s) {
        bytes_ += cost(s);
        items_.push_back(std::move(s));
    }
    std::string pop_front() {
        std::string s = std::move(items_.front());
        items_.pop_front();
        bytes_ -= cost(s);
        return s;
    }
    std::string pop_back() {
        std::string s = std::move(items_.back());
        items_.pop_back();
        bytes_ -= cost(s);
        return s;
    }

    [[nodiscard]] const std::string& operator[](std::size_t i) const { return items_[i]; }
    [[nodiscard]] std::size_t size() const noexcept { return items_.size(); }
    [[nodiscard]] bool empty() const noexcept { return items_.empty(); }
    [[nodiscard]] const_iterator begin() const noexcept { return items_.begin(); }
    [[nodiscard]] const_iterator end() const noexcept { return items_.end(); }
    [[nodiscard]] std::size_t bytes() const noexcept { return sizeof(ListValue) + bytes_; }

    friend bool operator==(const ListValue& a, const ListValue& b) { return a.items_ == b.items_; }

private:
    // Moved-from strings are left empty by every standard library in practice,
    // but size() is read before the move so the accounting never depends on it.
    static std::size_t cost(const std::string& s) noexcept { return sizeof(std::string) + s.size(); }

    container items_;
    std::size_t bytes_ = 0;
};

/// A stored value. Adding a type means adding an alternative here; every
/// std::visit that is not exhaustive then fails to compile, which is the point.
using Value = std::variant<StringValue, ListValue>;

[[nodiscard]] inline std::string_view type_name(const Value& v) noexcept {
    return std::visit(overloaded{[](const StringValue&) { return std::string_view{"string"}; },
                                 [](const ListValue&) { return std::string_view{"list"}; }},
                      v);
}

/// Approximate bytes used by a value, including container overhead.
[[nodiscard]] inline std::size_t memory_usage(const Value& v) noexcept {
    return std::visit(overloaded{[](const StringValue& s) { return sizeof(std::string) + s.capacity(); },
                                 [](const ListValue& list) { return list.bytes(); }},
                      v);
}

/// Weigher used by the store: key + value + a fixed per-entry overhead that
/// approximates the list node, hash bucket and bookkeeping.
struct EntryWeigher {
    static constexpr std::size_t kEntryOverhead = 96;
    std::size_t operator()(const std::string& key, const Value& value) const noexcept {
        return kEntryOverhead + key.size() + memory_usage(value);
    }
};

}  // namespace emberkv
