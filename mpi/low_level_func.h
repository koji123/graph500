/*
 * low_level_func.h
 *
 *  Created on: 2012/10/17
 *      Author: ueno
 */

#ifndef LOW_LEVEL_FUNC_H_
#define LOW_LEVEL_FUNC_H_

#include "parameters.h"

struct LocalPacket {
	enum {
		TOP_DOWN_LENGTH = PRM::PACKET_LENGTH/sizeof(uint32_t),
		BOTTOM_UP_LENGTH = PRM::PACKET_LENGTH/sizeof(TwodVertex)
	};
	int length;
	int64_t src;
	union {
		uint32_t t[TOP_DOWN_LENGTH];
		TwodVertex b[BOTTOM_UP_LENGTH];
	} data;
};

void backward_isolated_edge(
	int half_bitmap_width,
	int phase_bmp_off,
	BitmapType* __restrict__ phase_bitmap,
	const BitmapType* __restrict__ row_bitmap,
	const BitmapType* __restrict__ shared_visited,
	const TwodVertex* __restrict__ row_sums,
	const TwodVertex* __restrict__ isolated_edges,
	const int64_t* __restrict__ row_starts,
	const TwodVertex* __restrict__ edge_array,
	LocalPacket* buffer
);

#endif /* LOW_LEVEL_FUNC_H_ */
