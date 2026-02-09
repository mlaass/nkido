#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace cedar {

/// Non-owning view into a contiguous byte buffer
struct MemoryView {
    const std::uint8_t* data = nullptr;
    std::size_t size = 0;

    constexpr MemoryView() = default;
    constexpr MemoryView(const std::uint8_t* d, std::size_t s) : data(d), size(s) {}

    /// Implicit conversion from vector
    MemoryView(const std::vector<std::uint8_t>& v) : data(v.data()), size(v.size()) {}

    [[nodiscard]] constexpr bool empty() const { return size == 0 || data == nullptr; }
    [[nodiscard]] constexpr const std::uint8_t* begin() const { return data; }
    [[nodiscard]] constexpr const std::uint8_t* end() const { return data + size; }
};

/// Owning byte buffer with move semantics
class OwnedBuffer {
public:
    OwnedBuffer() = default;
    explicit OwnedBuffer(std::vector<std::uint8_t> data) : data_(std::move(data)) {}

    OwnedBuffer(OwnedBuffer&&) = default;
    OwnedBuffer& operator=(OwnedBuffer&&) = default;

    OwnedBuffer(const OwnedBuffer&) = delete;
    OwnedBuffer& operator=(const OwnedBuffer&) = delete;

    /// Get a non-owning view of the buffer
    [[nodiscard]] MemoryView view() const {
        return {data_.data(), data_.size()};
    }

    /// Take ownership of the underlying vector
    [[nodiscard]] std::vector<std::uint8_t> take() {
        return std::move(data_);
    }

    [[nodiscard]] bool empty() const { return data_.empty(); }
    [[nodiscard]] std::size_t size() const { return data_.size(); }
    [[nodiscard]] const std::uint8_t* data() const { return data_.data(); }

private:
    std::vector<std::uint8_t> data_;
};

}  // namespace cedar
