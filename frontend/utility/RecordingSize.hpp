#pragma once

#include <cstdint>
#include <optional>
#include <utility>

inline std::optional<std::pair<uint32_t, uint32_t>> MatchedRecordingSize(uint32_t width, uint32_t height, int left,
									 int top, int right, int bottom)
{
	if (left < 0 || top < 0 || right < 0 || bottom < 0 || width > 32768 || height > 32768) {
		return std::nullopt;
	}
	int64_t croppedWidth = int64_t(width) - left - right;
	int64_t croppedHeight = int64_t(height) - top - bottom;
	if (croppedWidth < 32 || croppedHeight < 32) {
		return std::nullopt;
	}
	// Match the encoder alignment used by obs_reset_video without trimming the image.
	return std::pair<uint32_t, uint32_t>{uint32_t((croppedWidth + 3) & ~3), uint32_t((croppedHeight + 1) & ~1)};
}
