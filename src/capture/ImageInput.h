#pragma once
#include "providers/ContentTranslation.h"
#include <span>
namespace sat {
constexpr size_t MaxInputImageBytes = 20 * 1024 * 1024;
TranslationAttachment ImportImage(std::span<const unsigned char> bytes, std::string name,
                                  std::stop_token stop);
TranslationAttachment ImportImageFile(const std::filesystem::path& path, std::stop_token stop);
} // namespace sat
