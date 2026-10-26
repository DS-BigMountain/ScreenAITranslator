#pragma once
#include "overlay/Overlay.h"
#include "providers/Provider.h"

namespace sat {
enum class TranslationPhase { Requesting, Layout };
using ProviderFactory = std::function<std::unique_ptr<ITranslationProvider>(const std::string&)>;

struct PreparedTranslation {
    TranslationResult result;
    std::vector<OverlayBlock> blocks;
    long long ocrMs{}, apiMs{}, totalMs{};
    bool textOnly{};
    size_t unrecognizedLines{}, lowConfidenceRegions{};
};

// Runs entirely on the worker. The callback reports phases without touching UI.
// Cancellation and errors propagate to the controller; no dialogs are shown here.
PreparedTranslation PrepareTranslation(Image image, RECT region, const Monitor& monitor,
                                       const Settings& options, const std::vector<ContextItem>& history,
                                       const std::string& key, std::stop_token stop,
                                       const ProviderFactory& createProvider,
                                       const std::function<void(TranslationPhase)>& phaseChanged);
} // namespace sat
