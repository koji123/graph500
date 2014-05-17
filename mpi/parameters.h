/*
 * parameters.h
 *
 *  Created on: Mar 2, 2012
 *      Author: koji
 */

#ifndef PARAMETERS_H_
#define PARAMETERS_H_

// for the systems that contains NUMA nodes
#define NUMA_BIND 0
#define CPU_BIND_CHECK 0
#define PRINT_BINDING 0
#define SHARED_MEMORY 0

// Switching the task assignment for the main thread and the sub thread
// 0: MPI is single mode: Main -> MPI, Sub: OpenMP
// 1: MPI is funneled mode: Main -> OpenMP, Sub: MPI
// Since communication and computation is overlapped, we cannot have main thread do both tasks.
#define MPI_FUNNELED 1

#define VERVOSE_MODE 1
#define PROFILING_MODE 1
#define DETAILED_PROF_MODE 0
#define REPORT_GEN_RPGRESS 1
#define ENABLE_FUJI_PROF 1

#define BFELL 0

// Optimization for CSR
#define ISOLATE_FIRST_EDGE 1
#define DEGREE_ORDER 0
#define DEGREE_ORDER_ONLY_IE 0
#define CONSOLIDATE_IFE_PROC 1

// We omit initialize predecessor array when this option is enabled.
// WARNING: In the most case, BFS generates correct answer without initializing predecessor array
// because all the vertexes reached in the previous would be reached in the current run.
// But this is not true in the general case. BFS may generate wrong answer in some situation.
#define INIT_PRED_ONCE 0

// Optimization for backward communication sub steps.
#define STREAM_UPDATE 1
#define BF_DEEPER_ASYNC 1

#define PRE_EXEC_TIME 300 // 300 seconds

#define VERTEX_SORTING 0 // do not support backward search
#define LOW_LEVEL_FUNCTION 1

// org = 1000
#define DENOM_TOPDOWN_TO_BOTTOMUP 2000
#define DENOM_BITMAP_TO_LIST 2.0

// Validation Level: 0: No validation, 1: validate at first time only, 2: validate all results
// Note: To conform to the specification, you must set 2
#define VALIDATION_LEVEL 2

#define CUDA_ENABLED 0
#define CUDA_COMPUTE_EXCLUSIVE_THREAD_MODE 1
#define CUDA_CHECK_PRINT_RANK

// atomic level of scanning Shared Visited
// 0: no atomic operation
// 1: non atomic read and atomic write
// 2: atomic read-write
#define SV_ATOMIC_LEVEL 0

#define SIMPLE_FOLD_COMM 1
#define DEBUG_PRINT 0
#define KD_PRINT 0

#define DISABLE_CUDA_CONCCURENT 0
#define NETWORK_PROBLEM_AYALISYS 0

#define SGI_OMPLACE_BUG 1

#ifdef __FUJITSU

#else // #ifdef __FUJITSU
#	undef ENABLE_FUJI_PROF
#	define ENABLE_FUJI_PROF 0
#endif // #ifdef __FUJITSU

#if VTRACE
#	undef REPORT_GEN_RPGRESS
#	define REPORT_GEN_RPGRESS 0
#endif // #if VTRACE

#if BFELL
#	undef ISOLATE_FIRST_EDGE
#	define ISOLATE_FIRST_EDGE 0
#	undef DEGREE_ORDER
#	define DEGREE_ORDER 0
#endif

#define CACHE_LINE 128
#define PAGE_SIZE 8192

#define IMD_OUT stderr

typedef uint8_t SortIdx;
typedef uint64_t BitmapType;
typedef uint32_t TwodVertex;

#ifdef __cplusplus
namespace PRM { //
#endif // #ifdef __cplusplus

#define SIZE_OF_SUMMARY_IS_EQUAL_TO_WARP_SIZE

enum {
	NUM_BFS_ROOTS = 64, // spec: 64
#if CUDA_ENABLED
	PACKET_LENGTH = 256,
	LOG_PACKET_LENGTH = 8,
#else
	PACKET_LENGTH = 1024,
#endif
	COMM_BUFFER_SIZE = 32*1024, // !!IMPORTANT VALUE!!
	PRE_ALLOCATE_COMM_BUFFER = 14,
	SEND_BUFFER_LIMIT = 6,

	BOTTOM_UP_BUFFER = 8,

	PREFETCH_DIST = 16,

	LOG_BIT_SCAN_TABLE = 11,
	LOG_NBPE = 6,
	LOG_BFELL_SORT = 8,

	// non-parameters
	NBPE = 1 << LOG_NBPE, // <= sizeof(BitmapType)*8
	NBPE_MASK = NBPE - 1,
	BFELL_SORT = 1 << LOG_BFELL_SORT,
	BFELL_SORT_MASK = BFELL_SORT - 1,

	USERSEED1 = 2,
	USERSEED2 = 3,

	TOP_DOWN_FOLD_TAG = 0,
	BOTTOM_UP_WAVE_TAG = 1,
	BOTTOM_UP_PRED_TAG = 2,
};

#ifdef __cplusplus
} // namespace PRM {

namespace GPU_PARAMS {
enum {
	LOG_WARP_SIZE = 5, // 32
	LOG_WARPS_PER_BLOCK = 3, // 8
	LOOPS_PER_THREAD = 8,
	ACTIVE_THREAD_BLOCKS = 64,
	NUMBER_CUDA_STREAMS = 4,

	// Dynamic Scheduling
	DS_TILE_SCALE = 16,
	DS_BLOCK_SCALE = 8,
	BLOCKS_PER_LAUNCH_RATE = 8*1024, // for debug

	EXPAND_DECODE_BLOCK_LENGTH = 16*1024*1024, // 64MB
	EXPAND_STREAM_BLOCK_LENGTH = EXPAND_DECODE_BLOCK_LENGTH * 2, // 32MB
	EXPAND_PACKET_LIST_LENGTH = EXPAND_DECODE_BLOCK_LENGTH / PRM::PACKET_LENGTH * 2,

	LOG_PACKING_EDGE_LISTS = 5, // 2^5 = 32 // for debug
	LOG_CQ_SUMMARIZING = LOG_WARP_SIZE, // 32 ( number of threads in a warp )

	// non-parameters
	WARP_SIZE = 1 << LOG_WARP_SIZE,
	WARPS_PER_BLOCK = 1 << LOG_WARPS_PER_BLOCK,
	LOG_THREADS_PER_BLOCK = LOG_WARP_SIZE + LOG_WARPS_PER_BLOCK,
	THREADS_PER_BLOCK = WARP_SIZE*WARPS_PER_BLOCK,
	// Number of max threads launched by 1 GPU kernel.
	MAX_ACTIVE_WARPS = WARPS_PER_BLOCK*ACTIVE_THREAD_BLOCKS,
	TEMP_BUFFER_LINES = MAX_ACTIVE_WARPS*LOOPS_PER_THREAD,

	READ_GRAPH_OUTBUF_SIZE = THREADS_PER_BLOCK * BLOCKS_PER_LAUNCH_RATE * 2,
//	READ_GRAPH_OUTBUF_SIZE = THREADS_PER_BLOCK * BLOCKS_PER_LAUNCH_RATE / 2,

	NUMBER_PACKING_EDGE_LISTS = (1 << LOG_PACKING_EDGE_LISTS),
	NUMBER_CQ_SUMMARIZING = (1 << LOG_CQ_SUMMARIZING),
};
} // namespace GPU_PARAMS {
#endif // #ifdef __cplusplus

#endif /* PARAMETERS_H_ */
