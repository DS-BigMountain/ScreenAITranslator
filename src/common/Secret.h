#pragma once
#include "Types.h"

namespace sat {
// Own a temporary credential and clear its buffer before releasing storage.
struct Secret {
    std::string value;
    explicit Secret(std::string text) : value(std::move(text)) {}
    ~Secret() {
        if (!value.empty())
            SecureZeroMemory(value.data(), value.size());
    }
    Secret(const Secret&) = delete;
    Secret& operator=(const Secret&) = delete;
};
} // namespace sat
