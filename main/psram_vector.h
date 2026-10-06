#ifndef PSRAM_VECTOR_H
#define PSRAM_VECTOR_H

#include <esp_heap_caps.h>
#include <esp_log.h>

#include <cstddef>
#include <cstdlib>
#include <vector>

template <typename T>
class PsramAllocator {
public:
    using value_type = T;

    PsramAllocator() noexcept = default;

    template <typename U>
    PsramAllocator(const PsramAllocator<U>&) noexcept {}

    T* allocate(std::size_t count) {
        if (count == 0) {
            return nullptr;
        }

        void* ptr = heap_caps_malloc(count * sizeof(T), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (ptr == nullptr) {
            ESP_LOGW("PsramAllocator",
                     "PSRAM allocation failed for %zu bytes; falling back to internal RAM",
                     count * sizeof(T));
            ptr = malloc(count * sizeof(T));
        }
        if (ptr == nullptr) {
            ESP_LOGE("PsramAllocator", "Failed to allocate %zu bytes", count * sizeof(T));
            abort();
        }
        return static_cast<T*>(ptr);
    }

    void deallocate(T* ptr, std::size_t) noexcept { heap_caps_free(ptr); }
};

template <typename T, typename U>
bool operator==(const PsramAllocator<T>&, const PsramAllocator<U>&) noexcept {
    return true;
}

template <typename T, typename U>
bool operator!=(const PsramAllocator<T>& lhs, const PsramAllocator<U>& rhs) noexcept {
    return !(lhs == rhs);
}

template <typename T>
using PsramVector = std::vector<T, PsramAllocator<T>>;

#endif  // PSRAM_VECTOR_H
