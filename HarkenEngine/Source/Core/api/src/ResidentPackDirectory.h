#pragma once

#include "harken/media/PackLogicImage.h"

namespace harken {
VnResult VisitDirectoryForActivation(const VnResidentPackVisitDescV1* desc);
// Only private byte spans pinned for the duration of the current directory visit are repeatable.
bool IsResidentDirectorySource(VnPackByteProviderFn provide, VnPackByteReleaseFn release, void* context) noexcept;
bool TakeResidentDirectoryImage(const VnPackLoadDesc& source, const VnPackByteSpan& span,
    const detail::PackDecodeBudget& budget, PackCanonicalImage& image, std::uint64_t& hash);
}
