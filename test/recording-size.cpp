#include <utility/RecordingSize.hpp>

#include <cassert>
#include <climits>

// clang++ -std=c++17 -Wall -Wextra -Werror -I frontend test/recording-size.cpp -o /tmp/recording-size-test
int main()
{
	assert((MatchedRecordingSize(1920, 1080, 0, 0, 0, 0) == std::pair<uint32_t, uint32_t>{1920, 1080}));
	assert((MatchedRecordingSize(1001, 777, 0, 0, 0, 0) == std::pair<uint32_t, uint32_t>{1004, 778}));
	// Retina Chrome page: crop browser chrome and a docked developer tools pane.
	assert((MatchedRecordingSize(2880, 1800, 0, 240, 600, 0) == std::pair<uint32_t, uint32_t>{2280, 1560}));
	assert((MatchedRecordingSize(32767, 32767, 0, 0, 0, 0) == std::pair<uint32_t, uint32_t>{32768, 32768}));
	assert(!MatchedRecordingSize(0, 1080, 0, 0, 0, 0));
	assert(!MatchedRecordingSize(1920, 1080, 0, 1050, 0, 0));
	assert(!MatchedRecordingSize(1920, 1080, 1900, 0, 1900, 0));
	assert(!MatchedRecordingSize(1920, 1080, -1, 0, 0, 0));
	assert(!MatchedRecordingSize(1920, 1080, INT_MAX, 0, INT_MAX, 0));
	assert(!MatchedRecordingSize(UINT_MAX, UINT_MAX, 0, 0, 0, 0));
}
