#ifndef NP_REMOTE_SESSION_PAIR_H
#define NP_REMOTE_SESSION_PAIR_H

#include <stdint.h>

enum np_remote_pair_state {
	NP_REMOTE_PAIR_INCOMPLETE = 0,
	NP_REMOTE_PAIR_MATCHED = 1,
	NP_REMOTE_PAIR_MISMATCHED = 2,
};

static inline enum np_remote_pair_state
np_remote_pair_tokens(uint64_t surface_token, uint64_t media_token)
{
	if (surface_token == 0 || media_token == 0)
		return NP_REMOTE_PAIR_INCOMPLETE;
	return surface_token == media_token
		? NP_REMOTE_PAIR_MATCHED : NP_REMOTE_PAIR_MISMATCHED;
}

#endif
