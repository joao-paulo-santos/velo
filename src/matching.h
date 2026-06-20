#ifndef MATCHING_H
#define MATCHING_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

enum matching_algorithm {
	MATCHING_ALGORITHM_NORMAL,
	MATCHING_ALGORITHM_PREFIX,
	MATCHING_ALGORITHM_FUZZY
};

int32_t match_words(enum matching_algorithm algorithm, const char *restrict patterns, const char *restrict str);

/*
 * Match-highlight support. A bounded list of byte ranges (indices into str,
 * half-open [start, end)) describing which bytes of str the patterns matched.
 * Used by the renderer to recolour the matched glyphs; match_words() above is
 * unaffected and stays the fast scoring path. See doc/palette.md.
 */
#define MATCH_RANGE_MAX 256
struct match_range { size_t start; size_t end; };
struct match_positions {
	size_t n;
	struct match_range ranges[MATCH_RANGE_MAX];
};

/*
 * Compute the matched byte ranges of str against patterns, using the same
 * algorithm semantics as match_words(). Returns true iff every space-separated
 * pattern word matches (i.e. str would be shown as a result); *out is then
 * filled with the union of ranges. Safe to call only for visible rows: the
 * fuzzy path walks the search tree a second time and is never invoked when
 * match-highlight is off.
 */
bool match_positions(enum matching_algorithm algorithm,
                     const char *restrict patterns,
                     const char *restrict str,
                     struct match_positions *out);

#endif /* MATCHING_H */
