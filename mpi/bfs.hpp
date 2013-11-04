/*
 * bfs.hpp
 *
 *  Created on: Mar 5, 2012
 *      Author: koji
 */

#ifndef BFS_HPP_
#define BFS_HPP_

#include <pthread.h>

#include <deque>

#if CUDA_ENABLED
#include "gpu_host.hpp"
#endif

#include "utils.hpp"
#include "double_linked_list.h"
#include "fiber.hpp"

namespace bfs_detail {
using namespace BFS_PARAMS;

enum VARINT_BFS_KIND {
	VARINT_FOLD,
	VARINT_EXPAND_CQ, // current queue
	VARINT_EXPAND_SV, // shared visited
};

struct FoldCommBuffer {
	uint32_t* stream;
	// info
	uint8_t complete_flag; // flag is set when send or receive complete. 0: plist, 1: vlist
	int target; // rank to which send or from which receive
	int length;
	ListEntry free_link;
	ListEntry extra_buf_link;
};

struct FoldPacket {
	int length; // init = 0
	int64_t src; // init = -1;
	uint32_t data[PACKET_LENGTH];
};

template <typename TwodVertex>
class BfsAsyncCommumicator
{
public:
	class EventHandler {
	public:
		virtual ~EventHandler() { }
		virtual void fold_received(FoldCommBuffer* data) = 0;
		virtual void fold_finish() = 0;
	};

	class Communicatable {
	public:
		virtual ~Communicatable() { }
		virtual void comm() = 0;
	};

	BfsAsyncCommumicator(EventHandler* event_handler, bool cuda_enabled, FiberManager* fiber_man__)
		: event_handler_(event_handler)
		, cuda_enabled_(cuda_enabled)
		, fiber_man_(fiber_man__)
	{
		d_ = new DynamicDataSet();
		pthread_mutex_init(&d_->thread_sync_, NULL);
		pthread_cond_init(&d_->thread_state_,  NULL);
		d_->cleanup_ = false;
		d_->command_active_ = false;
		d_->suspended_ = false;
		d_->terminated_ = false;
#if PROFILING_MODE
		d_->num_extra_buffer_ = 0;
#endif
		initializeListHead(&d_->free_buffer_);
		initializeListHead(&d_->extra_buffer_);

		// check whether the size of CompressedStream is page-aligned
		// which v1_list of FoldCommBuffer needs to page-align. allocate_fold_comm_buffer
		const int foldcomm_width = roundup<CACHE_LINE>(sizeof(FoldCommBuffer));
		const int foldbuf_width = sizeof(uint32_t) * BULK_TRANS_SIZE;

		comm_length_ =
#if BFS_BACKWARD
				std::max(mpi.size_2dc, mpi.size_2dr);
#else
				mpi.size_2dc;
#endif
#if VERVOSE_MODE
		if(mpi.isMaster()) {
			fprintf(IMD_OUT, "Allocating communication buffer (%zu * %d bytes)\n",
					(size_t)foldbuf_width*PRE_ALLOCATE_COMM_BUFFER*comm_length_, mpi.size_2d);
		}
#endif
		buffer_.comm_ = cache_aligned_xcalloc(foldcomm_width*PRE_ALLOCATE_COMM_BUFFER*comm_length_);
		buffer_.fold_ = page_aligned_xcalloc((size_t)foldbuf_width*PRE_ALLOCATE_COMM_BUFFER*comm_length_);
		node_ = (FoldNode*)malloc(sizeof(node_[0])*comm_length_);
		node_comm_ = (FoldNodeComm*)malloc(sizeof(node_comm_[0])*comm_length_);
		mpi_reqs_ = (MPI_Request*)malloc(sizeof(mpi_reqs_[0])*comm_length_*REQ_TOTAL);

#if CUDA_ENABLED
		if(cuda_enabled_) {
			CudaStreamManager::begin_cuda();
			CUDA_CHECK(cudaHostRegister(buffer_.fold_, foldbuf_width*PRE_ALLOCATE_COMM_BUFFER*mpi.size_2dc, cudaHostRegisterPortable));
			CudaStreamManager::end_cuda();
		}
#endif

		d_->num_send_reserved_buffer_ = 0;
		d_->num_recv_reserved_buffer_ = 0;
		for(int i = 0; i < comm_length_; ++i) {
			pthread_mutex_init(&node_[i].send_mutex, NULL);

			initializeListHead(&node_[i].sending_buffer);

			FoldCommBuffer *buf[PRE_ALLOCATE_COMM_BUFFER];
			for(int k = 0; k < PRE_ALLOCATE_COMM_BUFFER; ++k) {
				buf[k] = (FoldCommBuffer*)((uint8_t*)buffer_.comm_ + foldcomm_width*(i*PRE_ALLOCATE_COMM_BUFFER + k));
				buf[k]->stream = (FoldEdge*)((uint8_t*)buffer_.fold_ + foldbuf_width*(i*PRE_ALLOCATE_COMM_BUFFER + k));
				buf[k]->length = 0;
				initializeListEntry(&buf[k]->free_link);
				initializeListEntry(&buf[k]->extra_buf_link);
			}

			node_[i].current = NULL;
			for(int k = 0; k < PRE_ALLOCATE_COMM_BUFFER; ++k) {
				listInsertBack(&d_->free_buffer_, &buf[k]->free_link);
			}

			for(int k = 0; k < REQ_TOTAL; ++k) {
				mpi_reqs_[REQ_TOTAL*i + k] = MPI_REQUEST_NULL;
			}
			node_comm_[i].recv_buffer = NULL;
			node_comm_[i].send_buffer = NULL;
		}

		// initial value
		d_->comm_ = mpi.comm_2dc;
		d_->comm_size_ = mpi.size_2dr;

		pthread_create(&d_->thread_, NULL, comm_thread_routine_, this);
	}

	virtual ~BfsAsyncCommumicator()
	{
		if(!d_->cleanup_) {
			d_->cleanup_ = true;
			pthread_mutex_lock(&d_->thread_sync_);
			d_->terminated_ = true;
			d_->suspended_ = true;
			d_->command_active_ = true;
			pthread_mutex_unlock(&d_->thread_sync_);
			pthread_cond_broadcast(&d_->thread_state_);
			pthread_join(d_->thread_, NULL);
			pthread_mutex_destroy(&d_->thread_sync_);
			pthread_cond_destroy(&d_->thread_state_);

			for(int i = 0; i < comm_length_; ++i) {
				pthread_mutex_destroy(&node_[i].send_mutex);
			}

			release_extra_buffer();

#if CUDA_ENABLED
			if(cuda_enabled_) {
				CudaStreamManager::begin_cuda();
				CUDA_CHECK(cudaHostUnregister(buffer_.fold_));
				CudaStreamManager::end_cuda();
			}
#endif

			free(buffer_.comm_); buffer_.comm_ = NULL;
			free(buffer_.fold_); buffer_.fold_ = NULL;
			free(node_); node_ = NULL;
			free(node_comm_); node_comm_ = NULL;
			free(mpi_reqs_); mpi_reqs_ = NULL;

			delete d_; d_ = NULL;
		}
	}

	void release_extra_buffer() {
		while(listIsEmpty(&d_->extra_buffer_) == false) {
			FoldCommBuffer* sb = CONTAINING_RECORD(d_->extra_buffer_.fLink,
					FoldCommBuffer, extra_buf_link);
			listRemove(&sb->extra_buf_link);
#if CUDA_ENABLED
			if(cuda_enabled_) {
				CudaStreamManager::begin_cuda();
				CUDA_CHECK(cudaHostUnregister(sb));
				CudaStreamManager::end_cuda();
			}
#endif
			free(sb);
		}
#if PROFILING_MODE
		d_->num_extra_buffer_ = 0;
#endif
	}

	void begin_comm(bool forward_or_backward)
	{
#if !BFS_BACKWARD
		assert (forward_or_backward);
#endif
		CommCommand cmd;
		cmd.kind = SEND_START;
		d_->forward_or_backward_ = forward_or_backward;
		if(forward_or_backward) { // forward
			d_->comm_ = mpi.comm_2dc;
			d_->comm_size_ = mpi.size_2dr;
		}
		else { // backward
			d_->comm_ = mpi.comm_2dr;
			d_->comm_size_ = mpi.size_2dc;
		}
		put_command(cmd);
	}

	template <bool need_lock>
	FoldCommBuffer* lock_buffer(int target, int length) {
		FoldNode& dest_node = node_[target];
		while(1) {
			if(need_lock) pthread_mutex_lock(&dest_node.send_mutex);
			if(dest_node.current == NULL) {
				dest_node.current = get_buffer(true /* send buffer */);
				if(dest_node.current == NULL) {
					if(need_lock) pthread_mutex_unlock(&dest_node.send_mutex);
					while(fiber_man_->process_task(1)); // process recv task
					continue;
				}
			}
			FoldCommBuffer* sb = dest_node.current;
			if(sb->length + length > BULK_TRANS_SIZE)  {
				assert (sb->length <= BULK_TRANS_SIZE);
				send_submit(target);
				sb = dest_node.current = get_buffer(true /* send buffer */);
				if(sb == NULL) {
					if(need_lock) pthread_mutex_unlock(&dest_node.send_mutex);
					while(fiber_man_->process_task(1)); // process recv task
					continue;
				}
			}
			return sb;
		}
	}

	/**
	 * Asynchronous send.
	 * When the communicator receive data, it will call fold_received(FoldCommBuffer*) function.
	 * To reduce the memory consumption, when the communicator detects stacked jobs,
	 * it also process the tasks in the fiber_man_ except the tasks that have the lowest priority (0).
	 * This feature realize the fixed memory consumption.
	 */
	void send(uint32_t* stream, int length, int target)
	{
		assert(length > 0);

		FoldCommBuffer* sb = lock_buffer<true>(target, length);
		FoldNode& dest_node = node_[target];
		// add to send buffer
		memcpy(sb->stream + sb->target, stream, sizeof(stream[0])*target);
		sb->length += length;
		pthread_mutex_unlock(&dest_node.send_mutex);
	}

	void send_end(int target)
	{
#if PROFILING_MODE
		profiling::TimeKeeper tk_wait;
#endif
		FoldNode& dest_node = node_[target];
		FoldCommBuffer* sb = lock_buffer<true>(target, BULK_TRANS_SIZE);
		assert(sb->length == 0);
		send_submit(target);
		dest_node.current = NULL;
		pthread_mutex_unlock(&dest_node.send_mutex);
#if PROFILING_MODE
		comm_time_ += tk_wait;
#endif
	}

	void input_command(Communicatable* comm)
	{
		CommCommand cmd;
		cmd.kind = MANUAL_COMM;
		cmd.cmd = comm;
		put_command(cmd);
	}

	void relase_buffer(FoldCommBuffer* buf)
	{
		put_buffer(false /* receive buffer */, buf);
	}
#if PROFILING_MODE
	void submit_prof_info(const char* str, int number) {
		comm_time_.submit(str, number);
	}
	void submit_mem_info() {
		profiling::g_pis.submitCounter(d_->num_extra_buffer_, "# of extra buffer", 0);
	}
#endif
#ifndef NDEBUG
	bool check_num_send_buffer() { return (d_->num_send_reserved_buffer_ == 0 && d_->num_recv_reserved_buffer_ == 0); }
#endif
private:
	EventHandler* event_handler_;
	bool cuda_enabled_;

	enum COMM_COMMAND {
		SEND_START,
		SEND,
	//	SEND_END,
		MANUAL_COMM,
	};

	struct CommCommand {
		COMM_COMMAND kind;
		union {
			// SEND
			int target;
			// COMM_COMMAND
			Communicatable* cmd;
		};
	};

	struct DynamicDataSet {
		// lock topology
		// FoldNode::send_mutex -> thread_sync_
		pthread_t thread_;
		pthread_mutex_t thread_sync_;
		pthread_cond_t thread_state_;

		bool cleanup_;

		// monitor : thread_sync_
		volatile bool command_active_;
		volatile bool suspended_;
		volatile bool terminated_;
		std::deque<CommCommand> command_queue_;
		ListEntry free_buffer_;

		// accessed by comm thread only
		ListEntry extra_buffer_;
		int num_send_reserved_buffer_;
		int num_recv_reserved_buffer_;
#if PROFILING_MODE
		int num_extra_buffer_;
#endif

		bool forward_or_backward_;
		MPI_Comm comm_;
		int comm_size_;
	//	int sending_count_;
	} *d_;

	struct FoldNode {
		pthread_mutex_t send_mutex;

		// monitor : send_mutex
		FoldCommBuffer* current;

		// monitor : thread_sync_
		ListEntry sending_buffer;
	};

	struct FoldNodeComm {
		FoldCommBuffer* recv_buffer;
		FoldCommBuffer* send_buffer;
	};

	enum MPI_REQ_INDEX {
		REQ_SEND = 0,
		REQ_RECV = 1,
		REQ_TOTAL = 2,
	};

	int comm_length_;
	FoldNode* node_;
	FoldNodeComm* node_comm_;
	FiberManager* fiber_man_;
	std::deque<int> recv_stv;

	// accessed by communication thread only
	MPI_Request* mpi_reqs_;

	struct {
		void* comm_;
		void* fold_;
	} buffer_;
#if PROFILING_MODE
	profiling::TimeSpan comm_time_;
#endif

	static void* comm_thread_routine_(void* pThis) {
		static_cast<BfsAsyncCommumicator*>(pThis)->comm_thread_routine();
		pthread_exit(NULL);
		return NULL;
	}
	void comm_thread_routine()
	{
		int num_recv_active = 0, num_send_active = 0;

		// command loop
		while(true) {
			if(d_->command_active_) {
				pthread_mutex_lock(&d_->thread_sync_);
				CommCommand cmd;
				while(pop_command(&cmd)) {
					pthread_mutex_unlock(&d_->thread_sync_);
					switch(cmd.kind) {
					case SEND_START:
						for(int i = 0; i < d_->comm_size_; ++i) {
							FoldCommBuffer* rb = get_buffer(false /* receive buffer */);
							assert (rb != NULL);
							set_receive_buffer(i, rb);
						}
						num_send_active = num_recv_active = d_->comm_size_;
						break;
					case SEND:
						set_send_buffer(cmd.target);
						break;
						/*
					case SEND_END:
						if(num_recv_active == 0 && num_send_active == 0) {
							assert (d_->num_send_reserved_buffer_ == comm_length_);
							event_handler_->finish();
						}
						break;
						*/
					case MANUAL_COMM:
						cmd.cmd->comm();
						break;
					}
					pthread_mutex_lock(&d_->thread_sync_);
				}
				pthread_mutex_unlock(&d_->thread_sync_);
			}
			if(num_recv_active == 0 && num_send_active == 0) {
				pthread_mutex_lock(&d_->thread_sync_);
				if(d_->command_active_ == false) {
					d_->suspended_ = true;
					if( d_->terminated_ ) { pthread_mutex_unlock(&d_->thread_sync_); break; }
					pthread_cond_wait(&d_->thread_state_, &d_->thread_sync_);
				}
				pthread_mutex_unlock(&d_->thread_sync_);
			}

			int index;
			int flag;
			MPI_Status status;
			MPI_Testany(d_->comm_size_ * (int)REQ_TOTAL, mpi_reqs_, &index, &flag, &status);

			if(flag == 0 || index == MPI_UNDEFINED) {
				continue;
			}

			const int src_c = index/REQ_TOTAL;
			const MPI_REQ_INDEX req_kind = (MPI_REQ_INDEX)(index%REQ_TOTAL);
			const bool b_send = (req_kind == REQ_SEND);

			FoldNodeComm& comm_node = node_comm_[src_c];
			FoldCommBuffer* buf = b_send ? comm_node.send_buffer : comm_node.recv_buffer;

			assert (mpi_reqs_[index] == MPI_REQUEST_NULL);
			mpi_reqs_[index] = MPI_REQUEST_NULL;

			switch(req_kind) {
			case REQ_RECV:
				{
					MPI_Get_count(&status, MpiTypeOf<uint32_t>::type, &buf->length);
#if VERVOSE_MODE
					g_recv += buf->length * sizeof(uint32_t);
#endif
				}
				break;
			default:
				break;
			}

			bool completion_message = (buf->length == 0);
			// complete
			if(b_send) {
				// send buffer
				comm_node.send_buffer = NULL;
				put_buffer(true /* send buffer */, buf);
				if(completion_message) {
					// sent fold completion
					--num_send_active;
					if(num_recv_active == 0 && num_send_active == 0) {
						event_handler_->fold_finish();
					}
				}
				else {
					set_send_buffer(src_c);
				}
			}
			else {
				// recv buffer
				if(completion_message) {
					// received fold completion
					--num_recv_active;
					node_comm_[src_c].recv_buffer = NULL;
					put_buffer(false /* receive buffer */, buf);
					if(num_recv_active == 0 && num_send_active == 0) {
						event_handler_->fold_finish();
					}
				}
				else {
					// received both plist and vlist
					// set new buffer for next receiving
					recv_stv.push_back(src_c);

					event_handler_->fold_received(buf);
				}
			}

			// process recv starves
			while(recv_stv.size() > 0) {
				FoldCommBuffer *rb = get_buffer(false /* receive buffer */);
				if(rb == NULL) break;
				set_receive_buffer(recv_stv.front(), rb);
				recv_stv.pop_front();
			}

		}
	}

	bool pop_command(CommCommand* cmd) {
		if(d_->command_queue_.size()) {
			*cmd = d_->command_queue_[0];
			d_->command_queue_.pop_front();
			return true;
		}
		d_->command_active_ = false;
		return false;
	}

	void put_command(CommCommand& cmd)
	{
		bool command_active;

		pthread_mutex_lock(&d_->thread_sync_);
		d_->command_queue_.push_back(cmd);
		command_active = d_->command_active_;
		if(command_active == false) d_->command_active_ = true;
		pthread_mutex_unlock(&d_->thread_sync_);

		if(command_active == false) pthread_cond_broadcast(&d_->thread_state_);
	}

	FoldCommBuffer* allocate_buffer()
	{
		const int edges_offset = roundup<PAGE_SIZE>(sizeof(FoldCommBuffer));
		const int mem_length = edges_offset + BULK_TRANS_SIZE * sizeof(uint32_t);
		uint8_t* new_buffer = (uint8_t*)page_aligned_xcalloc(mem_length);
#if CUDA_ENABLED
		if(cuda_enabled_) {
			CudaStreamManager::begin_cuda();
			CUDA_CHECK(cudaHostRegister(new_buffer, mem_length, cudaHostRegisterPortable));
			CudaStreamManager::end_cuda();
		}
#endif
		FoldCommBuffer* r = (FoldCommBuffer*)new_buffer;
		r->stream = (uint32_t*)(new_buffer + edges_offset);
		initializeListEntry(&r->free_link);
		initializeListEntry(&r->extra_buf_link);
#if 0
		memset(r->v0_stream, 0, sizeof(CompressedStream));
		memset(r->v1_list, 0, BLOCK_V1_LENGTH*sizeof(r->v1_list[0]));
		memset(r, 0, sizeof(FoldCommBuffer));
		r->v0_stream = (CompressedStream*)(new_buffer + v0_offset);
		r->v1_list = (uint32_t*)(new_buffer + v1_offset);
#endif
		return r;
	}

	bool is_send_buffer_available() {
		// # of remaining sending buffer
		if(d_->num_send_reserved_buffer_ >= comm_length_ * MAX_EXTRA_SEND_BUFFER)
			return false;
		// # of recv tasks
		if(d_->num_recv_reserved_buffer_ - comm_length_ > 100)
			return false;
		// do not permit allocate new buffer for send
		if(listIsEmpty(&d_->free_buffer_))
			return false;
		return true;
	}

	FoldCommBuffer* get_buffer(bool send_or_recv)
	{
		pthread_mutex_lock(&d_->thread_sync_);
		if(send_or_recv) {
			if(!is_send_buffer_available()) {
				pthread_mutex_unlock(&d_->thread_sync_);
				return NULL;
			}
			++(d_->num_send_reserved_buffer_);
		}
		else {
			++(d_->num_recv_reserved_buffer_);
		}
#if 1
		while(listIsEmpty(&d_->free_buffer_)) {
			pthread_mutex_unlock(&d_->thread_sync_);
			fiber_man_->process_task(1);
			pthread_mutex_lock(&d_->thread_sync_);
		}
#else
		if(listIsEmpty(&d_->free_buffer_)) {
			pthread_mutex_unlock(&d_->thread_sync_);
			FoldCommBuffer* new_buffer = allocate_buffer();
			pthread_mutex_lock(&d_->thread_sync_);
			listInsertBack(&d_->extra_buffer_, &new_buffer->extra_buf_link);
#if PROFILING_MODE
			d_->num_extra_buffer_++;
#endif
			pthread_mutex_unlock(&d_->thread_sync_);
			return new_buffer;
		}
#endif
		FoldCommBuffer* buffer = CONTAINING_RECORD(d_->free_buffer_.fLink,
				FoldCommBuffer, free_link);
		listRemove(&buffer->free_link);
		pthread_mutex_unlock(&d_->thread_sync_);
		return buffer;
	}

	void put_buffer(bool send_or_recv, FoldCommBuffer* buf)
	{
		buf->length = 0;
		pthread_mutex_lock(&d_->thread_sync_);
		listInsertBack(&d_->free_buffer_, &buf->free_link);
		if(send_or_recv) {
			--(d_->num_send_reserved_buffer_);
		}
		else {
			--(d_->num_recv_reserved_buffer_);
		}
		pthread_mutex_unlock(&d_->thread_sync_);
	}

	void set_receive_buffer(int target, FoldCommBuffer *rb)
	{
		FoldNodeComm& comm_node = node_comm_[target];
		MPI_Request* recv_reqs = &mpi_reqs_[REQ_TOTAL*target + REQ_RECV];

		comm_node.recv_buffer = rb;
		MPI_Irecv(rb->stream, BULK_TRANS_SIZE,
				MpiTypeOf<uint32_t>::type, target, COMM_V0_TAG, d_->comm_, recv_reqs);

		rb->complete_flag = 0;
	}

	void set_send_buffer(int target)
	{
		FoldNode& node = node_[target];
		FoldNodeComm& comm_node = node_comm_[target];
		FoldCommBuffer* sb = NULL;

		if(comm_node.send_buffer) {
			return ;
		}

		pthread_mutex_lock(&d_->thread_sync_);
		if(listIsEmpty(&node.sending_buffer) == false) {
			sb = CONTAINING_RECORD(node.sending_buffer.fLink, FoldCommBuffer, free_link);
			listRemove(&sb->free_link);
		}
		pthread_mutex_unlock(&d_->thread_sync_);

		if(sb) {
			assert (sb->length <= BULK_TRANS_SIZE);
			comm_node.send_buffer = sb;
			MPI_Isend(sb->stream, sb->length,
					MpiTypeOf<uint32_t>::type, target, COMM_V0_TAG, d_->comm_, &mpi_reqs_[REQ_TOTAL*target + REQ_SEND]);

			sb->complete_flag = 0;
		}

	}

	void send_submit(int target)
	{
		FoldNode& dest_node = node_[target];
		FoldCommBuffer* sb = dest_node.current;
		sb->target = target;
	#if 0
		ffprintf(IMD_OUT, stderr, "send[from=%d,to=%d,npacket=%d,ptr_packet_index=%d,vlist_length=%d]\n",
				rank, sb->target, sb->npacket, sb->ptr_packet_index, sb->vlist_length);
	#endif

		CommCommand cmd;
		cmd.kind = SEND;
		cmd.target = target;
		bool command_active;

		pthread_mutex_lock(&d_->thread_sync_);
		listInsertBack(&dest_node.sending_buffer, &sb->free_link);
		d_->command_queue_.push_back(cmd);
		command_active = d_->command_active_;
		if(command_active == false) d_->command_active_ = true;
		pthread_mutex_unlock(&d_->thread_sync_);

		if(command_active == false) pthread_cond_broadcast(&d_->thread_state_);
	}
};

} // namespace detail {


template <typename TwodVertex, typename PARAMS>
class BfsBase
	: private bfs_detail::BfsAsyncCommumicator::EventHandler
{
public:
	typedef typename PARAMS::BitmapType BitmapType;
	typedef BfsBase<TwodVertex, PARAMS> ThisType;
	typedef Graph2DCSR<TwodVertex> GraphType;
	enum {
		// Number of CQ bitmap entries represent as 1 bit in summary.
		// Since the type of bitmap entry is int32_t and 1 cache line is composed of 32 bitmap entries,
		// 32 is effective value.
		ENABLE_WRITING_DEPTH = 1,

		BUCKET_UNIT_SIZE = 1024,

		EXPAND_COMM_THRESOLD_AVG = 10, // 50%
		EXPAND_COMM_THRESOLD_MAX = 75, // 75%
		EXPAND_COMM_DENOMINATOR = 100,

		// non-parameters
		NBPE = sizeof(BitmapType)*8
	};

	class QueuedVertexes {
	public:
		TwodVertex v[BUCKET_UNIT_SIZE];
		int length;
		enum { SIZE = BUCKET_UNIT_SIZE };

		QueuedVertexes() : length(0) { }
		bool append(TwodVertex val) {
			if(length == SIZE) return false;
			v[length++] = val; return true;
		}
		void append_nocheck(TwodVertex val) {
			v[length++] = val;
		}
		void append_nocheck(TwodVertex pred, TwodVertex tgt) {
			v[length+0] = pred;
			v[length+1] = tgt;
			length += 2;
		}
		bool full() { return (length == SIZE); }
		int size() { return length; }
		void clear() { length = 0; }
	};

	struct ThreadLocalBuffer {
		QueuedVertexes* cur_buffer; // TODO: initialize
		int local_size;
		FoldPacket fold_packet[1];
	};

	BfsBase(bool cuda_enabled)
		: comm_(this, cuda_enabled, &fiber_man_)
#if 0
		, recv_task_(65536)
#endif
		, cq_comm_(this, true)
		, visited_comm_(this, false)
	{
		//
	}

	virtual ~BfsBase()
	{
		//
	}

	template <typename EdgeList>
	void construct(EdgeList* edge_list)
	{
		// minimun requirement of CQ
		// CPU: MINIMUN_SIZE_OF_CQ_BITMAP words -> MINIMUN_SIZE_OF_CQ_BITMAP * NUMBER_PACKING_EDGE_LISTS * mpi.size_2dc
		// GPU: THREADS_PER_BLOCK words -> THREADS_PER_BLOCK * NUMBER_PACKING_EDGE_LISTS * mpi.size_2dc

		int log_min_vertices = get_msb_index(std::max<int>(BFELL_SORT, NBPE) * 2 * mpi.size_2d);

		detail::GraphConstructor2DCSR<TwodVertex, EdgeList> constructor;
		constructor.construct(edge_list, log_min_vertices, false /* sorting vertex */, graph_);

		log_local_bitmap_ = graph_.log_local_verts() - get_msb_index(NBPE);
	}

	void prepare_bfs() {
		printInformation();
		allocate_memory(graph_);
	}

	void run_bfs(int64_t root, int64_t* pred);

	void get_pred(int64_t* pred) {
	//	comm_.release_extra_buffer();
	}

	void end_bfs() {
		deallocate_memory();
	}

	GraphType graph_;

// protected:

	int64_t get_bitmap_size_src() const {
		return (int64_t(1) << log_local_bitmap_) * mpi.size_2dr;
	}
	int64_t get_bitmap_size_tgt() const {
		return (int64_t(1) << log_local_bitmap_) * mpi.size_2dc;
	}
	int64_t get_bitmap_size_local() const {
		return (int64_t(1) << log_local_bitmap_);
	}
	int64_t get_number_of_local_vertices() const {
		return (int64_t(1) << graph_.lgl_);
	}
	int64_t get_actual_number_of_local_vertices() const {
		return (int64_t(1) << graph_.log_actual_local_verts());
	}
	int64_t get_number_of_global_vertices() const {
		return (int64_t(1) << graph_.log_global_verts());
	}
	int64_t get_actual_number_of_global_vertices() const {
		return (int64_t(1) << graph_.log_actual_global_verts());
	}

	// virtual functions
	virtual int varint_encode(const int64_t* input, int length, uint8_t* output, bfs_detail::VARINT_BFS_KIND kind)
	{
		return varint_encode_stream_signed((const uint64_t*)input, length, output);
	}

	int64_t get_nq_threshold()
	{
		return sizeof(BitmapType) * graph_.get_bitmap_size_local() -
				sizeof(bfs_detail::PacketIndex) * (omp_get_max_threads()*2 + 16);
	}

	void compute_jobs_per_node(bool long_or_short, int num_dest_nodes, int& jobs_per_node, int64_t& chunk_size) {
		// long: 32KB chunk of CQ
		// short: 512KB chunk of CQ
		const int demon = long_or_short ? 8 : (8*16);
		const int maximum = get_blocks((long_or_short ? 2048 : 128), num_dest_nodes);
		const int summary_size_per_node = get_bitmap_size_local() / MINIMUN_SIZE_OF_CQ_BITMAP;
		// compute raw value
		int raw_value = std::max(1, summary_size_per_node / demon);
		// apply upper limit
		jobs_per_node = std::min<int>(raw_value, maximum);
		chunk_size = summary_size_per_node / jobs_per_node;
	}

	template <typename JOB_TYPE>
	void internal_init_job(JOB_TYPE* job, int64_t offset, int64_t chunk, int64_t summary_size) {
		const int64_t start = std::min(offset * chunk, summary_size);
		const int64_t end = std::min(start + chunk, summary_size);
		job->this_ = this;
		job->i_start_ = start;
		job->i_end_ = end;
	}

	void allocate_memory(Graph2DCSR<IndexArray, TwodVertex>& g)
	{
		const int max_threads = omp_get_max_threads();
	//	cq_bitmap_ = (BitmapType*)
	//			page_aligned_xcalloc(sizeof(cq_bitmap_[0])*get_bitmap_size_src());
		cq_summary_ = (BitmapType*)
				malloc(sizeof(cq_summary_[0])*get_summary_size_v0());
	//	shared_visited_ = (BitmapType*)
	//			page_aligned_xcalloc(sizeof(shared_visited_[0])*get_bitmap_size_tgt());
		nq_bitmap_ = (BitmapType*)
				page_aligned_xcalloc(sizeof(nq_bitmap_[0])*get_bitmap_size_local());
#if VERTEX_SORTING
		nq_sorted_bitmap_ = (BitmapType*)
				page_aligned_xcalloc(sizeof(nq_bitmap_[0])*get_bitmap_size_local());
#endif
		visited_ = (BitmapType*)
				page_aligned_xcalloc(sizeof(visited_[0])*get_bitmap_size_local());

		tmp_packet_max_length_ = sizeof(BitmapType) *
				get_bitmap_size_local() / BFS_PARAMS::PACKET_LENGTH + omp_get_max_threads()*2;
		tmp_packet_index_ = (bfs_detail::PacketIndex*)
				malloc(sizeof(bfs_detail::PacketIndex)*tmp_packet_max_length_);
#if BFS_EXPAND_COMPRESSION
		cq_comm_.local_buffer_ = (bfs_detail::CompressedStream*)
#else // #if BFS_EXPAND_COMPRESSION
		cq_comm_.local_buffer_ = (uint32_t*)
#endif // #if BFS_EXPAND_COMPRESSION
				page_aligned_xcalloc(sizeof(BitmapType)*get_bitmap_size_local());
#if VERTEX_SORTING
#if BFS_EXPAND_COMPRESSION
		visited_comm_.local_buffer_ = (bfs_detail::CompressedStream*)
#else // #if BFS_EXPAND_COMPRESSION
		visited_comm_.local_buffer_ = (uint32_t*)
#endif // #if BFS_EXPAND_COMPRESSION
				page_aligned_xcalloc(sizeof(BitmapType)*get_bitmap_size_local());
#else // #if VERTEX_SORTING
		visited_comm_.local_buffer_ = cq_comm_.local_buffer_;
#endif // #if VERTEX_SORTING
		cq_comm_.recv_buffer_ =
				page_aligned_xcalloc(sizeof(BitmapType)*get_bitmap_size_src());
		visited_comm_.recv_buffer_ =
				page_aligned_xcalloc(sizeof(BitmapType)*get_bitmap_size_tgt());

		thread_local_buffer_ = (ThreadLocalBuffer**)
				malloc(sizeof(thread_local_buffer_[0])*max_threads);
		d_ = (DynamicDataSet*)malloc(sizeof(d_[0]));
#if AVOID_BUSY_WAIT
		pthread_mutex_init(&d_->avoid_busy_wait_sync_, NULL);
#endif

		comm_length_ =
#if BFS_BACKWARD
				std::max(mpi.size_2dc, mpi.size_2dr);
#else
				mpi.size_2dc;
#endif
		const int buffer_width = roundup<CACHE_LINE>(
				sizeof(ThreadLocalBuffer) + sizeof(FoldPacket) * comm_length_);
		buffer_.thread_local_ = cache_aligned_xcalloc(buffer_width*max_threads);
		for(int i = 0; i < max_threads; ++i) {
			thread_local_buffer_[i] = (ThreadLocalBuffer*)
					((uint8_t*)buffer_.thread_local_ + buffer_width*i);
		}

		// compute job length
		const int num_nodes = mpi.size_2dr;
		const int node_idx = mpi.rank_2dr;
		int long_job_blocks, short_job_blocks;
		int64_t long_job_chunk, short_job_chunk;
		compute_jobs_per_node(true, num_nodes, long_job_blocks, long_job_chunk);
		sched_.long_job_length = long_job_blocks * num_nodes;
		compute_jobs_per_node(false, num_nodes, short_job_blocks, short_job_chunk);
		sched_.short_job_length = short_job_blocks * num_nodes;

		assert (long_job_blocks*long_job_chunk*num_nodes >= get_summary_size_v0());
		assert (short_job_blocks*short_job_chunk*num_nodes >= get_summary_size_v0());

		sched_.long_job = new ExtractEdge[sched_.long_job_length];
		sched_.short_job = new ExtractEdge[sched_.short_job_length];
#if BFS_BACKWARD
		sched_.back_long_job = new BackwardExtractEdge[sched_.long_job_length];
		sched_.back_short_job = new BackwardExtractEdge[sched_.short_job_length];
#endif
		const int64_t summary_size_v0 = get_summary_size_v0();
		assert ((sched_.long_job_length % long_job_blocks) == 0);
		assert ((sched_.long_job_length % long_job_blocks) == 0);
		for(int i = 0; i < num_nodes; ++i) {
#if 1
			int scrambled_node_idx = (i + node_idx) % num_nodes;
#else
			int scrambled_node_idx = i;
#endif
			for(int j = 0; j < long_job_blocks; ++j) {
				int i_node_major = i * long_job_blocks + j; // for forward jobs
				int i_block_major = j * num_nodes + scrambled_node_idx; // for backward jobs
				internal_init_job(&sched_.long_job[i_node_major], i_node_major, long_job_chunk, summary_size_v0);
#if BFS_BACKWARD
				internal_init_job(&sched_.back_long_job[i_block_major], i_node_major, long_job_chunk, summary_size_v0);
#endif
			}
			for(int j = 0; j < short_job_blocks; ++j) {
				int i_node_major = i * short_job_blocks + j; // for forward jobs
				int i_block_major = j * num_nodes + scrambled_node_idx; // for backward jobs
				internal_init_job(&sched_.short_job[i_node_major], i_node_major, short_job_chunk, summary_size_v0);
#if BFS_BACKWARD
				internal_init_job(&sched_.back_short_job[i_block_major], i_node_major, short_job_chunk, summary_size_v0);
#endif
			}
		}

		sched_.fold_end_job = new ExtractEnd[comm_length_];
		for(int i = 0; i < comm_length_; ++i) {
			sched_.fold_end_job[i].this_ = this;
			sched_.fold_end_job[i].target_ = i;
		}

#if 0
		num_recv_tasks_ = max_threads * 3;
		for(int i = 0; i < num_recv_tasks_; ++i) {
			recv_task_.push(new ReceiverProcessing(this));
		}
#endif
#if BIT_SCAN_TABLE
		bit_scan_table_[0] = BFS_PARAMS::BIT_SCAN_TABLE_BITS;
		for(int i = 1; i < BFS_PARAMS::BIT_SCAN_TABLE_SIZE; ++i) {
			for(int b = 0; b < BFS_PARAMS::BIT_SCAN_TABLE_BITS; ++b) {
				if(i & (1 << b)) {
					bit_scan_table_[i] = b + 1;
					break;
				}
			}
		}
#endif
	}

	void deallocate_memory()
	{
		free(cq_bitmap_); cq_bitmap_ = NULL;
		free(cq_summary_); cq_summary_ = NULL;
		free(shared_visited_); shared_visited_ = NULL;
		free(nq_bitmap_); nq_bitmap_ = NULL;
#if VERTEX_SORTING
		free(nq_sorted_bitmap_); nq_sorted_bitmap_ = NULL;
#endif
		free(visited_); visited_ = NULL;
		free(tmp_packet_index_); tmp_packet_index_ = NULL;
		free(cq_comm_.local_buffer_); cq_comm_.local_buffer_ = NULL;
#if VERTEX_SORTING
		free(visited_comm_.local_buffer_); visited_comm_.local_buffer_ = NULL;
#endif
		free(cq_comm_.recv_buffer_); cq_comm_.recv_buffer_ = NULL;
		free(visited_comm_.recv_buffer_); visited_comm_.recv_buffer_ = NULL;
		free(thread_local_buffer_); thread_local_buffer_ = NULL;
#if AVOID_BUSY_WAIT
		pthread_mutex_destroy(&d_->avoid_busy_wait_sync_);
#endif
		free(d_); d_ = NULL;
		delete [] sched_.long_job; sched_.long_job = NULL;
		delete [] sched_.short_job; sched_.short_job = NULL;
#if BFS_BACKWARD
		delete [] sched_.back_long_job; sched_.back_long_job = NULL;
		delete [] sched_.back_short_job; sched_.back_short_job = NULL;
#endif
		delete [] sched_.fold_end_job; sched_.fold_end_job = NULL;
		free(buffer_.thread_local_); buffer_.thread_local_ = NULL;
#if 0
		for(int i = 0; i < num_recv_tasks_; ++i) {
			delete recv_task_.pop();
		}
#endif
	}

	void initialize_memory(int64_t* pred)
	{
		using namespace BFS_PARAMS;
		const int64_t num_local_vertices = get_actual_number_of_local_vertices();
		const int64_t bitmap_size_visited = get_bitmap_size_local();
		const int64_t bitmap_size_v1 = get_bitmap_size_tgt();
		const int64_t summary_size = get_summary_size_v0();

		BitmapType* cq_bitmap = cq_bitmap_;
		BitmapType* cq_summary = cq_summary_;
		BitmapType* nq_bitmap = nq_bitmap_;
#if VERTEX_SORTING
		BitmapType* nq_sorted_bitmap = nq_sorted_bitmap_;
#endif
		BitmapType* visited = visited_;
		BitmapType* shared_visited = shared_visited_;

#pragma omp parallel
		{
#if 1	// Only Spec2010 needs this initialization
#pragma omp for nowait
			for(int64_t i = 0; i < num_local_vertices; ++i) {
				pred[i] = -1;
			}
#endif
			// clear NQ and visited
#pragma omp for nowait
			for(int64_t i = 0; i < bitmap_size_visited; ++i) {
				nq_bitmap[i] = 0;
#if VERTEX_SORTING
				nq_sorted_bitmap[i] = 0;
#endif
				visited[i] = 0;
			}
			// clear CQ and CQ summary
#pragma omp for nowait
			for(int64_t i = 0; i < summary_size; ++i) {
				cq_summary[i] = 0;
				for(int k = 0; k < MINIMUN_SIZE_OF_CQ_BITMAP; ++k) {
					cq_bitmap[i*MINIMUN_SIZE_OF_CQ_BITMAP + k] = 0;
				}
			}
			// clear shared visited
#pragma omp for nowait
			for(int64_t i = 0; i < bitmap_size_v1; ++i) {
				shared_visited[i] = 0;
			}

			// clear fold packet buffer
			FoldPacket* packet_array = thread_local_buffer_[omp_get_thread_num()]->fold_packet;
			for(int i = 0; i < comm_length_; ++i) {
				packet_array[i].num_edges = 0;
			}
		}
	}

	//-------------------------------------------------------------//
	// expand phase
	//-------------------------------------------------------------//

	template <typename T>
	void get_visited_pointers(T** ptrs, int num_ptrs, void* visited_buf) {
		int half_bitmap_width = get_bitmap_size_local() / 2;
		for(int i = 0; i < num_ptrs; ++i) {
			ptrs[i] = (T*)((BitmapType*)visited_buf + half_bitmap_width);
		}
	}

	template <typename CVT>
	TwodVertex* top_down_make_nq_list(int& size, CVT cvt) {
		int num_threads = omp_get_max_threads();
		int th_offset[num_threads+1] = {0};
		int num_buffers = nq_.stack_.size();
		TwodVertex* flatten;
		TwodVertex high_cmask = TwodVertex(mpi.rank_2dc) << graph_.lgl_;
#pragma omp parallel
		{
			int tid = omp_get_thread_num();
			int size = 0;
#pragma omp for schedule(static) nowait
			for(int i = 0; i < num_buffers; ++i) {
				size += nq_.stack_[i]->length;
			}
			th_offset[tid+1] = size;
#pragma omp barrier
#pragma omp single
			{
				for(int i = 0; i < num_threads; ++i) {
					th_offset[i+1] += th_offset[i];
				}
				flatten = (TwodVertex*)malloc(th_offset[num_threads]*sizeof(TwodVertex));
			} // implicit barrier
			int offset = th_offset[tid];
#pragma omp for schedule(static) nowait
			for(int i = 0; i < num_buffers; ++i) {
				int len = nq_.stack_[i]->length;
				cvt(flatten + offset, nq_.stack_[i]->v, len);
				nq_.stack_[i]->length = 0;
				offset += len;
			}
			assert (offset == th_offset[tid+1]);
		} // implicit barrier
		for(int i = 0; i < num_buffers; ++i) {
			// Since there are no need to lock pool when the free memory is added,
			// we invoke Pool::free method explicitly.
			nq_empty_buffer_.memory::Pool<QueuedVertexes>::free(nq_.stack_[i]);
		}
		size = th_offset[num_threads];
		return flatten;
	}

	void allgather_nq_without_compression(TwodVertex* nq, int nq_size, MPI_Comm comm, int comm_size) {
		int recv_size[comm_size];
		int recv_off[comm_size+1];
		MPI_Allgather(&nq_size, 1, MPI_INT, recv_size, 1, MPI_INT, comm);
		recv_off[0] = 0;
		for(int i = 0; i < comm_size; ++i) {
			recv_off[i+1] = recv_off[i] + recv_size[0];
		}
		cq_size_ = recv_off[comm_size];
		TwodVertex* recv_buf = (TwodVertex*)((cq_size_*sizeof(TwodVertex) > cq_buf_size_) ?
				(cq_extra_buf_ = malloc(cq_size_*sizeof(TwodVertex))) :
				cq_buf_);
		MPI_Allgatherv(nq, nq_size, MpiTypeOf<TwodVertex>::type,
				recv_buf, recv_size, recv_off, MpiTypeOf<TwodVertex>::type, comm);
		cq_list_ = recv_buf;
	}

	void top_down_expand() {
		// expand NQ within a processor column
		// convert NQ to a SRC format
		struct LocalToSrc {
			const TwodVertex high_cmask = TwodVertex(mpi.rank_2dc) << graph_.lgl_;
			void operator ()(TwodVertex* dst, TwodVertex* src, int length) const {
				for(int i = 0; i < length; ++i) {
					dst[i] = src[i] | high_cmask;
				}
			}
		};
		int nq_size;
		TwodVertex* flatten = top_down_make_nq_list(nq_size, LocalToSrc());
		allgather_nq_without_compression(flatten, nq_size, mpi.comm_2dr, mpi.size_2dc );
	}
	void top_down_switch_expand() {
		// expand NQ within a processor row
		// convert NQ to a DST format
		struct LocalToDst {
			const TwodVertex high_rmask = TwodVertex(mpi.rank_2dr) << graph_.lgl_;
			void operator ()(TwodVertex* dst, TwodVertex* src, int length) const {
				for(int i = 0; i < length; ++i) {
					dst[i] = src[i] | high_rmask;
				}
			}
		};
		int nq_size;
		TwodVertex* flatten = top_down_make_nq_list(nq_size, LocalToDst());
		allgather_nq_without_compression(flatten, nq_size, mpi.comm_2dc, mpi.size_2dr);

		// update bitmap
		// TODO: initialize shared_visited_
#pragma omp parallel for
		for(int i = 0; i < cq_size_; ++i) {
			TwodVertex v = cq_list_[i];
			TwodVertex word_idx = v / NBPE;
			int bit_idx = v % NBPE;
			BitmapType mask = BitmapType(1) << bit_idx;
			__sync_fetch_and_or(&shared_visited_[word_idx], mask); // TODO: this is slow ?
		}
	}

	int bottom_up_make_nq_list(bool with_z, TwodVertex shifted_rc, TwodVertex* outbuf) {
		const int max_threads = omp_get_max_threads();
		const int bitmap_width = get_bitmap_size_local();
		int node_nq_size;

		int size_z = with_z ? mpi.size_z : 1;
		int rank_z = with_z ? mpi.rank_z : 0;

		if(bitmap_or_list_) {
			const int node_threads = max_threads * size_z;
			//int *th_count = s_->count;
			int *th_offset = s_->offset;
#pragma omp parallel
			{
				int tid = omp_get_thread_num() + max_threads * rank_z;
				int count = 0;
#pragma omp for nowait
				for(int i = 0; i < bitmap_width; ++i) {
					count += __builtin_popcount(new_visited_[i] & ~(old_visited_[i]));
				}
				th_offset[tid+1] = count;
#pragma omp barrier
#pragma omp single
				{
					if(with_z) s_->sync.barrier();
					if(rank_z == 0) {
						th_offset[0] = 0;
						for(int i = 0; i < node_threads; ++i) {
							th_offset[i+1] += th_offset[i];
						}
						assert (th_offset[node_threads] <= bitmap_width*sizeof(BitmapType)/sizeof(TwodVertex)*2);
					}
					if(with_z) s_->sync.barrier();
					node_nq_size = th_offset[node_threads];
				} // implicit barrier

				TwodVertex* dst = outbuf + th_offset[tid];
				//TwodVertex r_shifted = TwodVertex(mpi.rank_2dr) << graph_.lgl_;
#pragma omp for nowait
				for(int i = 0; i < bitmap_width; ++i) {
					BitmapType bmp_i = new_visited_[i] & ~(old_visited_[i]);
					while(bmp_i != 0) {
						TwodVertex bit_idx = __builtin_ctz(bmp_i);
						*(dst++) = (i * NBPE + bit_idx) | shifted_rc;
						bmp_i &= bmp_i - 1;
					}
				}
				assert ((dst - buf) == (th_offset[tid+1] - th_offset[tid]));
			} // implicit barrier
		}
		else {
			const int num_parts = 2 * size_z;
			TwodVertex* new_vis[2]; get_visited_pointers(new_vis, 2, new_visited_);
			//int *th_count = s_->count;
			int *part_offset = s_->offset;
			for(int i = 0; i < 2; ++i) {
				part_offset[rank_z*2+1+i] = new_visited_list_size_[i];
			}
			if(with_z) s_->sync.barrier();
			if(rank_z == 0) {
				part_offset[0] = 0;
				for(int i = 0; i < num_parts; ++i) {
					part_offset[i+1] += part_offset[i];
				}
				assert (part_offset[num_parts] <= bitmap_width*sizeof(BitmapType)/sizeof(TwodVertex)*2);
				node_nq_size = part_offset[num_parts];
			}
			if(with_z) s_->sync.barrier();

			TwodVertex local_mask = (TwodVertex(1) << graph_.lgl_) - 1;
			//TwodVertex r_shifted = TwodVertex(mpi.rank_2dr) << graph_.lgl_;
			for(int i = 0; i < 2; ++i) {
				TwodVertex* dst = outbuf + part_offset[rank_z*2+i];
				TwodVertex* src = new_vis[i];
				TwodVertex size = new_visited_list_size_[i];

#pragma omp parallel for if(node_nq_size*sizeof(TwodVertex) > 16*1024*mpi.size_z)
				for(int c = 0; c < size; ++c) {
					dst[c] = (src[c] & local_mask) | shifted_rc;
				}
			}
		}

		return node_nq_size;
	}

	void bottom_up_expand_nq_list() {
		assert (mpi.isYdimAvailable() || (cq_buf_orig_ == cq_buf_));
		int node_nq_size = bottom_up_make_nq_list(
				mpi.isYdimAvailable(), TwodVertex(mpi.rank_2dr) << graph_.lgl_, (TwodVertex*)cq_buf_orig_);

		if(mpi.rank_z == 0) {
			s_->count[0] = MpiCol::allgatherv((TwodVertex*)cq_buf_orig_,
					 (TwodVertex*)nq_recv_buf_, node_nq_size, mpi.comm_y, mpi.size_y);
		}
		if(mpi.isYdimAvailable()) s_->sync.barrier();
		int recv_nq_size = s_->count[0];

#pragma omp parallel if(recv_nq_size > 1024*16)
		{
			const int max_threads = omp_get_num_threads();
			const int node_threads = max_threads * mpi.size_z;
			int64_t num_node_verts = get_number_of_local_vertices() * mpi.size_z;
			int64_t min_value = num_node_verts * mpi.rank_y;
			int64_t max_value = min_value + num_node_verts;
			int tid = omp_get_thread_num() + max_threads * mpi.rank_z;
			TwodVertex* begin, end;
			get_partition(recv_nq_size, nq_recv_buf_, min_value, max_value,
					NBPE, node_threads, tid, begin, end);

			for(TwodVertex* ptr = begin; ptr < end; ++ptr) {
				TwodVertex dst = *ptr;
				TwodVertex word_idx = dst / NBPE;
				int bit_idx = dst % NBPE;
				shared_visited_[word_idx] |= BitmapType(1) << bit_idx;
			}
		} // implicit barrier
		if(mpi.isYdimAvailable()) s_->sync.barrier();
	}

#if !STREAM_UPDATE
	void bottom_up_update_pred() {
		ScatterContext scatter(mpi.comm_2dr);
		int comm_size = comm_size;

		TwodVertex* send_buf, recv_buf;

#pragma omp parallel
		{
			int* counts = scatter.get_counts();
			int num_bufs = nq_.stack_.size();
			int lgl = graph_.lgl_;
#pragma omp for
			for(int b = 0; b < num_bufs; ++b) {
				int length = nq_.stack_[b]->length;
				TwodVertex* data = nq_.stack_[b]->v;
				for(int i = 0; i < length; i += 2) {
					counts[data[i+1] >> lgl] += 2;
				}
			} // implicit barrier
#pragma omp single
			{
				scatter.sum();
				send_buf = (TwodVertex*)page_aligned_xcalloc(scatter.get_send_count()*sizeof(TwodVertex));
			} // implicit barrier

			int* offsets = scatter.get_offsets();
#pragma omp for
			for(int b = 0; b < num_bufs; ++b) {
				int length = nq_.stack_[b]->length;
				TwodVertex* data = nq_.stack_[b]->v;
				for(int i = 0; i < length; i += 2) {
					int dest = data[i+1] >> lgl;
					int off = offsets[dest]; offsets[dest] += 2;
					send_buf[off+0] = data[i+0]; // pred
					send_buf[off+1] = data[i+1]; // tgt
				}
			} // implicit barrier
#pragma omp single
			{
				recv_buf = scatter.scatter(send_buf);
				int *recv_offsets = scatter.get_recv_offsets();
				for(int i = 0; i < comm_size; ++i) {
					recv_offsets[i+1] /= 2;
				}
			} // implicit barrier

			int parts_per_blk = (omp_get_num_threads() * 4 + comm_size - 1) / comm_size;
			int num_parts = comm_size * parts_per_blk;

#pragma omp for
			for(int p = 0; p < num_parts; ++p) {
				int begin, end;
				get_partition(scatter.get_recv_offsets(), comm_size, p, parts_per_blk, begin, end);
				int lgl = graph_.lgl_;
				int lgsize = graph_.lgr_ + graph_.lgc_;
				TwodVertex lmask = (TwodVertex(1) << lgl) - 1;
				int64_t cshifted = int64_t(p / parts_per_blk) << graph_.lgr_;
				int64_t levelshifted = int64_t(current_level_) << 48;
				int64_t const_mask = cshifted | levelshifted;
				for(int i = begin; i < end; ++i) {
					TwodVertex pred_dst = recv_buf[i*2+0];
					TwodVertex tgt_local = recv_buf[i*2+1] & lmask;
					int64_t pred_v = ((pred_dst & lmask) << lgsize) | const_mask | (pred_dst >> lgl);
					assert (pred_[tgt_local] == -1);
					pred_[tgt_local] = pred_v;
				}
			} // implicit barrier

#pragma omp single nowait
			{
				scatter.free(recv_buf);
				free(send_buf);
			}
		}
	}
#endif

	void bottom_up_expand() {
#if !STREAM_UPDATE
		bottom_up_update_pred();
#endif
		int bitmap_width = get_bitmap_size_local();
		int max_capacity = vlq::BitmapEncoder::calc_capacity_of_values(
				bitmap_width, NBPE, bitmap_width*sizeof(BitmapType));
		int threashold = std::min<int>(max_capacity, bitmap_width*sizeof(BitmapType)/2);
		if(max_nq_size_ > threashold) {
			// bitmap
			assert (bitmap_or_list_);
			if(mpi.isYdimAvailable()) s_->sync.barrier();
			if(mpi.rank_z == 0) {
				BitmapType* const bitmap = new_visited_;
				BitmapType* recv_buffer = shared_visited_;
				int shared_bitmap_width = bitmap_width * mpi.size_z;
				MPI_Allgather(bitmap, shared_bitmap_width, get_mpi_type(bitmap[0]),
						recv_buffer, shared_bitmap_width, get_mpi_type(bitmap[0]), mpi.comm_y);
			}
			if(mpi.isYdimAvailable()) s_->sync.barrier();
#if VERVOSE_MODE
			g_bitmap_send += bitmap_width * sizeof(BitmapType);
			g_bitmap_recv += bitmap_width * mpi.size_2dr* sizeof(BitmapType);
#endif
		}
		else {
			// list
			bottom_up_expand_nq_list();
		}
		bitmap_or_list_ = (max_nq_size_ > threashold);
	}

	void bottom_up_switch_expand() {
#if !STREAM_UPDATE
		bottom_up_update_pred();
#endif
		int node_nq_size = bottom_up_make_nq_list(
				false, TwodVertex(mpi.rank_2dc) << graph_.lgl_, (TwodVertex*)nq_recv_buf_);

		cq_size_ = MpiCol::allgatherv((TwodVertex*)nq_recv_buf_,
				 (TwodVertex*)cq_buf_, node_nq_size, mpi.comm_2dr, mpi.size_2dc);

		bitmap_or_list_ = false;
	}

	//-------------------------------------------------------------//
	// top-down search
	//-------------------------------------------------------------//

	struct TopDownSender : public Runnable {
		virtual void run() {
			using namespace BFS_PARAMS;
#if PROFILING_MODE
			profiling::TimeKeeper tk_all;
			profiling::TimeKeeper tk_commit;
			profiling::TimeSpan ts_commit;
#endif
			//
			TwodVertex* cq_list = (TwodVertex*)this_->cq_bitmap_;
			TwodVertex cq_size = (TwodVertex*)this_->cq_size_;
			bfs_detail::FoldPacket* packet_array =
					this_->thread_local_buffer_[omp_get_thread_num()]->fold_packet;
			int lgl = graph_.log_local_verts();
			uint32_t local_mask = (uint32_t(1) << lgl) - 1;
#if VERVOSE_MODE
			int64_t num_edge_relax = 0;
#endif

			TwodVertex i_start, i_end;
			get_partition(cq_size, num_tasks_, task_idx_, i_start, i_end);
			for(TwodVertex i = i_start; i < i_end; ++i) {
				TwodVertex src = cq_list[i];
				TwodVertex word_idx = src / NBPE;
				int bit_idx = src % NBPE;
				BitmapType row_bitmap_i = this_->graph_.row_bitmap_[word_idx];
				BitmapType mask = BitmapType(1) << bit_idx;
				if(row_bitmap_i & mask) {
					uint32_t pred[2] = { (-(int64_t)src) >> 32, uint32_t(-(int64_t)src) };
					BitmapType low_mask = (BitmapType(1) << bit_idx) - 1;
					TwodVertex non_zero_off = this_->graph_.row_sums_[word_idx] +
							__builtin_popcount(this_->graph_.row_bitmap_[word_idx] & low_mask);
					SortIdx sorted_idx = this_->graph_.sorted_idx_[non_zero_off];
					typename GraphType::BlockOffset& blk_off = this_->graph_.blk_off[non_zero_off];
					SortIdx* blk_col_len = this_->graph_.col_len_ + blk_off.length_start;
					TwodVertex* edge_array = this_->graph_.edge_array_ + blk_off.edge_start;

					TwodVertex c = 0;
					for( ; sorted_idx < blk_col_len[c]; ++c, edge_array += blk_col_len[c]) {
						TwodVertex tgt = edge_array[sorted_idx];
						int dest = tgt >> lgl;
						uint32_t tgt_local = tgt & local_mask;
						bfs_detail::FoldPacket& pk = packet_array[dest];
						if(pk.length > PACKET_SIZE-3) { // low probability
#if PROFILING_MODE
							tk_commit.getSpanAndReset();
#endif
							this_->comm_.send(pk.data, pk.length, dest);
#if PROFILING_MODE
							ts_commit += tk_commit;
#endif
							pk.length = 0;
						}
						if(pk.src != src) { // TODO: use conditional branch
							pk.data[pk.length+0] = pred[0];
							pk.data[pk.length+1] = pred[1];
							pk.length += 2;
						}
						pk.data[pk.length] = tgt_local;
					}
#if VERVOSE_MODE
					num_edge_relax += c;
#endif
				}
			}
#if PROFILING_MODE
			profiling::TimeSpan ts_all;
			ts_all += tk_all;
			ts_all -= ts_commit;
			this_->extract_edge_time_ += ts_all;
			this_->commit_time_ += ts_commit;
#endif
			volatile int* jobs_ptr = &this_->d_->num_remaining_extract_jobs_;
			if(__sync_fetch_and_add(jobs_ptr, -1) == 1) {
				this_->fiber_man_.submit_array(this_->sched_.fold_end_job, mpi.size_2dc, 0);
			}
#if VERVOSE_MODE
			__sync_fetch_and_add(&this_->d_->num_edge_relax_, num_edge_relax);
#endif
		}
		ThisType* this_;
		int num_tasks_, task_idx_;
	};

	struct TopDownSendEnd : public Runnable {
		virtual void run() {
			// flush buffer
#if PROFILING_MODE
			profiling::TimeKeeper tk_all;
			profiling::TimeKeeper tk_commit;
			profiling::TimeSpan ts_commit;
#endif
			for(int i = 0; i < omp_get_num_threads(); ++i) {
				bfs_detail::FoldPacket* packet_array =
						this_->thread_local_buffer_[i]->fold_packet;
				bfs_detail::FoldPacket& pk = packet_array[target_];
				if(pk.length > 0) {
#if PROFILING_MODE
					tk_commit.getSpanAndReset();
#endif
					this_->comm_.send(pk.data, pk.length, target_);
#if PROFILING_MODE
					ts_commit += tk_commit;
#endif
					packet_array[target_].length = 0;
					packet_array[target_].src = -1;
				}
			}
#if PROFILING_MODE
			tk_commit.getSpanAndReset();
#endif
			this_->comm_.send_end(target_);
#if PROFILING_MODE
			ts_commit += tk_commit;
			profiling::TimeSpan ts_all;
			ts_all += tk_all;
			ts_all -= ts_commit;
			this_->extract_edge_time_ += ts_all;
			this_->commit_time_ += ts_commit;
#endif
		}
		ThisType* this_;
		int target_;
	};

	struct TopDownReceiver : public Runnable {
		TopDownReceiver(ThisType* this__, bfs_detail::FoldCommBuffer* data__)
			: this_(this__), data_(data__) 	{ }
		virtual void run() {
#if PROFILING_MODE
			profiling::TimeKeeper tk_all;
			profiling::TimeKeeper tk_commit;
#endif

			using namespace BFS_PARAMS;
			ThreadLocalBuffer* tlb = thread_local_buffer_[omp_get_thread_num()];
			QueuedVertexes* buf = tlb->cur_buffer;
			if(buf == NULL) buf = nq_empty_buffer_.get();
			BitmapType* visited = this_->new_visited_;
			int64_t* restrict const pred = this_->pred_;
			const int cur_level = this_->current_level_;
			uint32_t* stream = data_->stream;
			int length = data_->length;
			int64_t pred_v = -1;

			// for id converter //
			int lgr = this_->graph_.lgr_;
			int lgl = this_->graph_.lgl_;
			int lgsize = lgr + this_->graph_.lgc_;
			int64_t lmask = ((int64_t(1) << lgl) - 1);
			int r = data_->target;
			// ------------------- //

			for(int i = 0; i < length; ) {
				uint32_t v = stream[i];
				if(int32_t(v) < 0) {
					int64_t src = -((int64_t(v) << 32) | stream[i+1]);
					pred_v = ((src & lmask) << lgsize) | ((src >> lgl) << lgr) | int64_t(r) |
							(int64_t(cur_level) << 48);
					i += 2;
				}
				else {
					TwodVertex tgt_local = v;
					const TwodVertex word_idx = tgt_local / NBPE;
					const int bit_idx = tgt_local % NBPE;
					const BitmapType mask = BitmapType(1) << bit_idx;

					if((visited[word_idx] & mask) == 0) { // if this vertex has not visited
						if((__sync_fetch_and_or(&visited[word_idx], mask) & mask) == 0) {
							assert (pred[tgt_local] == -1);
							pred[tgt_local] = pred_v;
							if(buf->full()) {
								nq_.push(buf); buf = nq_empty_buffer_.get();
							}
							buf->append_nocheck(tgt_local);
						}
					}
					i += 1;
				}
			}
			tlb->cur_buffer = buf;
			this_->comm_.relase_buffer(data_);
#if PROFILING_MODE
			this_->recv_proc_time_ += tk_all;
#endif
			delete this;
		}
		ThisType* const this_;
		bfs_detail::FoldCommBuffer* data_;
	};

	virtual void fold_received(bfs_detail::FoldCommBuffer* data)
	{
		fiber_man_.submit(new TopDownReceiver(this, data), 1);
	}

	virtual void fold_finish()
	{
		fiber_man_.end_processing();
	}

	//-------------------------------------------------------------//
	// bottom-up search
	//-------------------------------------------------------------//

	void botto_up_print_stt(int64_t num_blocks, int64_t num_vertexes, int* nq_count) {
		int64_t send_stt[2] = { num_vertexes, num_blocks };
		int64_t sum_stt[2];
		int64_t max_stt[2];
		MPI_Reduce(send_stt, sum_stt, 2, MpiTypeOf<int64_t>::type, MPI_SUM, 0, mpi.comm_2d);
		MPI_Reduce(send_stt, max_stt, 2, MpiTypeOf<int64_t>::type, MPI_MAX, 0, mpi.comm_2d);
		if(mpi.isMaster()) {
			fprintf(IMD_OUT, "Bottom-Up using List. Total %f M Vertexes / %f M Blocks = %f Max %f %%+ Vertexes %f %%+ Blocks\n",
					to_mega(sum_stt[0]), to_mega(sum_stt[1]), to_mega(sum_stt[0]) / to_mega(sum_stt[1]),
					diff_percent(max_stt[0], sum_stt[0], mpi.size_2d),
					diff_percent(max_stt[1], sum_stt[1], mpi.size_2d));
		}
		int count_length = mpi.size_2dc;
		int start_proc = mpi.rank_2dc;
		int size_mask = mpi.size_2d - 1;
		int64_t phase_count[count_length];
		int64_t phase_recv[count_length];
		for(int i = 0; i < count_length; ++i) {
			phase_count[i] = nq_count[(start_proc + i) & size_mask];
		}
		MPI_Reduce(phase_count, phase_recv, count_length, MpiTypeOf<int64_t>::type, MPI_SUM, 0, mpi.comm_2d);
		if(mpi.isMaster()) {
			int64_t total_nq = 0;
			for(int i = 0; i < count_length; ++i) {
				total_nq += phase_recv[i];
			}
			fprintf(IMD_OUT, "Bottom-Up: %"PRId64" vertexes found. Break down ...\n", total_nq);
			for(int i = 0; i < count_length; ++i) {
				fprintf(IMD_OUT, "step %d / %d  %f M Vertexes ( %f %% )\n",
						i+1, count_length, to_mega(phase_recv[i]), (double)phase_recv[i] / (double)total_nq);
			}
		}
	}

	int bottom_up_search_bitmap_process_step(
			BitmapType* phase_bitmap,
			TwodVertex phase_bmp_off,
			TwodVertex half_bitmap_width)
	{
		struct BottomUpRow {
			SortIdx orig, sorted;
		};
		int num_bufs_nq_before = nq_.stack_.size();
		int visited_count = 0;
#pragma omp parallel reduction(+:visited_count)
		{
			ThreadLocalBuffer* tlb = thread_local_buffer_[omp_get_thread_num()];
			QueuedVertexes* buf = tlb->cur_buffer;
			if(buf == NULL) buf = nq_empty_buffer_.get();
			visited_count -= buf->length;

			TwodVertex num_blks = half_bitmap_width * NBPE / BFELL_SORT;
#pragma omp for
			for(TwodVertex blk_idx = 0; blk_idx < num_blks; ++blk_idx) {

				TwodVertex blk_bmp_off = blk_idx * BFELL_SORT / NBPE;
				BitmapType* blk_row_bitmap = graph_.row_bitmap_ + phase_bmp_off + blk_bmp_off;
				BitmapType* blk_bitmap = phase_bitmap + blk_bmp_off;
				TwodVertex* blk_row_sums = graph_.row_sums_ + phase_bmp_off + blk_bmp_off;
				SortIdx sorted_idx = graph_.sorted_idx_;
				BottomUpRow rows[BFELL_SORT];
				int num_active_rows = 0;

				for(int bmp_idx = 0; bmp_idx < BFELL_SORT / NBPE; ++bmp_idx) {
					BitmapType row_bmp_i = blk_row_bitmap[bmp_idx];
					BitmapType unvis_i = ~(blk_bitmap[bmp_idx]) & row_bmp_i;
					if(unvis_i == BitmapType(0)) continue;
					TwodVertex bmp_row_sums = blk_row_sums[bmp_idx];
					do {
						uint32_t visb_idx = __builtin_ctz(unvis_i);
						BitmapType mask = (BitmapType(1) << visb_idx) - 1;
						TwodVertex non_zero_idx = bmp_row_sums + __builtin_popcount(row_bmp_i & mask);
						rows[num_active_rows].orig = bmp_idx * NBPE + visb_idx;
						rows[num_active_rows].sorted = sorted_idx[non_zero_idx];
						++num_active_rows;
						unvis_i &= unvis_i - 1;
					} while(unvis_i != BitmapType(0));
				}

				TwodVertex phase_blk_off = phase_bmp_off * NBPE / BFELL_SORT;
				int64_t edge_offset = graph_.blk_off[phase_blk_off + blk_idx].edge_start;
				int64_t length_start = graph_.blk_off[phase_blk_off + blk_idx].length_start;
				TwodVertex* col_edge_array = graph_.edge_array_ + edge_offset;
				SortIdx* col_len = graph_.col_len_ + length_start;
				TwodVertex blk_vertex_base = phase_bmp_off * NBPE + blk_idx * BFELL_SORT;

				int c = 0;
				for( ; num_active_rows > 0; ++c) {
					SortIdx next_col_len = col_len[c + 1];
					int i = num_active_rows - 1;
	#if 0
					for( ; i >= 7; i -= 8) {
						BottomUpRow* cur_rows = &rows[i-7];
						TwodVertex src = {
								col_edge_array[cur_rows[0].sorted],
								col_edge_array[cur_rows[1].sorted],
								col_edge_array[cur_rows[2].sorted],
								col_edge_array[cur_rows[3].sorted],
								col_edge_array[cur_rows[4].sorted],
								col_edge_array[cur_rows[5].sorted],
								col_edge_array[cur_rows[6].sorted],
								col_edge_array[cur_rows[7].sorted],
						};
						bool connected = {
								shared_visited_[src[0] / NBPE] & (BitmapType(1) << (src[0] % NBPE)),
								shared_visited_[src[1] / NBPE] & (BitmapType(1) << (src[1] % NBPE)),
								shared_visited_[src[2] / NBPE] & (BitmapType(1) << (src[2] % NBPE)),
								shared_visited_[src[3] / NBPE] & (BitmapType(1) << (src[3] % NBPE)),
								shared_visited_[src[4] / NBPE] & (BitmapType(1) << (src[4] % NBPE)),
								shared_visited_[src[5] / NBPE] & (BitmapType(1) << (src[5] % NBPE)),
								shared_visited_[src[6] / NBPE] & (BitmapType(1) << (src[6] % NBPE)),
								shared_visited_[src[7] / NBPE] & (BitmapType(1) << (src[7] % NBPE)),
						};
						for(int s = 7; s >= 0; --s) {
							if(connected[s]) {
								// add to next queue
								int orig = cur_rows[s].orig;
								blk_bitmap[orig / NBPE] |= (BitmapType(1) << (orig % NBPE));
								if(buf->full()) {
									nq_.push(buf); buf = nq_empty_buffer_.get();
								}
								buf->append_nocheck(src[s], blk_vertex_base + orig);
								// end this row
								cur_rows[s] = rows[--num_active_rows];
							}
							else if(cur_rows[s].sorted >= next_col_len) {
								// end this row
								cur_rows[s] = rows[--num_active_rows];
							}
						}
					}
	#elif 0
					for( ; i >= 3; i -= 4) {
						BottomUpRow* cur_rows = &rows[i-3];
						TwodVertex src[4] = {
								col_edge_array[cur_rows[0].sorted],
								col_edge_array[cur_rows[1].sorted],
								col_edge_array[cur_rows[2].sorted],
								col_edge_array[cur_rows[3].sorted],
						};
						bool connected[4] = {
								shared_visited_[src[0] / NBPE] & (BitmapType(1) << (src[0] % NBPE)),
								shared_visited_[src[1] / NBPE] & (BitmapType(1) << (src[1] % NBPE)),
								shared_visited_[src[2] / NBPE] & (BitmapType(1) << (src[2] % NBPE)),
								shared_visited_[src[3] / NBPE] & (BitmapType(1) << (src[3] % NBPE)),
						};
						for(int s = 0; s < 4; ++s) {
							if(connected[s]) { // TODO: use conditional branch
								// add to next queue
								int orig = cur_rows[s].orig;
								blk_bitmap[orig / NBPE] |= (BitmapType(1) << (orig % NBPE));
								if(buf->full()) { // TODO: remove this branch
									nq_.push(buf); buf = nq_empty_buffer_.get();
								}
								buf->append_nocheck(src[s], blk_vertex_base + orig);
								// end this row
								cur_rows[s] = rows[--num_active_rows];
							}
							else if(cur_rows[s].sorted >= next_col_len) {
								// end this row
								cur_rows[s] = rows[--num_active_rows];
							}
						}
					}
	#endif
					for( ; i >= 0; --i) {
						SortIdx row = rows[i].sorted;
						TwodVertex src = col_edge_array[row];
						if(shared_visited_[src / NBPE] & (BitmapType(1) << (src % NBPE))) {
							// add to next queue
							int orig = rows[i].orig;
							blk_bitmap[orig / NBPE] |= (BitmapType(1) << (orig % NBPE));
							if(buf->full()) {
								nq_.push(buf); buf = nq_empty_buffer_.get();
							}
							buf->append_nocheck(src, blk_vertex_base + orig);
							// end this row
							rows[i] = rows[--num_active_rows];
						}
						else if(row >= next_col_len) {
							// end this row
							rows[i] = rows[--num_active_rows];
						}
					}
					col_edge_array += col_len[c];
				}
			} // #pragma omp for

			tlb->cur_buffer = buf;
			visited_count += buf->length;
		}
		int num_bufs_nq_after = nq_.stack_.size();
		for(int i = num_bufs_nq_before; i < num_bufs_nq_after; ++i) {
			visited_count += nq_.stack_[i]->length;
		}
		return visited_count;
	}

	TwodVertex bottom_up_search_list_process_step(
#if VERVOSE_MODE
			int64_t num_vertexes,
			int64_t num_blocks,
#endif
			TwodVertex* phase_list,
			TwodVertex phase_size,
			int8_t* vertex_enabled,
			TwodVertex* write_list,
			TwodVertex phase_bmp_off,
			TwodVertex half_bitmap_width)
	{
		ThreadLocalBuffer* tlb = thread_local_buffer_[omp_get_thread_num()];
		QueuedVertexes* buf = tlb->cur_buffer;
		if(buf == NULL) buf = nq_empty_buffer_.get();
		int num_enabled = phase_size;
#if VERVOSE_MODE
		num_vertexes += phase_size;
#endif

		struct BottomUpRow {
			SortIdx orig, sorted, orig_i;
		};

		for(int i = 0; i < phase_size; ) {
			int blk_i_start = i;
			TwodVertex blk_idx = phase_list[i] / BFELL_SORT;
			int num_active_rows = 0;
			BottomUpRow rows[BFELL_SORT];
			SortIdx* sorted_idx = graph_.sorted_idx_;
			TwodVertex* row_sums = graph_.row_sums_;
			BitmapType* row_bitmap = graph_.row_bitmap_;
#if VERVOSE_MODE
			num_blocks++;
#endif

			do {
				TwodVertex tgt = phase_list[i];
				TwodVertex word_idx = tgt / NBPE;
				int bit_idx = tgt % NBPE;
				BitmapType mask = (BitmapType(1) << bit_idx) - 1;
				TwodVertex non_zero_idx = row_sums[word_idx] +
						__builtin_popcount(row_bitmap[word_idx] & mask);
				rows[num_active_rows].orig = tgt % BFELL_SORT;
				rows[num_active_rows].orig_i = i - blk_i_start;
				rows[num_active_rows].sorted = sorted_idx[non_zero_idx];
				++num_active_rows;
				vertex_enabled[i] = 1;
			} while((phase_list[++i] / BFELL_SORT) == blk_idx);

			TwodVertex phase_blk_off = phase_bmp_off * NBPE / BFELL_SORT;
			int64_t edge_offset = graph_.blk_off[phase_blk_off + blk_idx].edge_start;
			int64_t length_start = graph_.blk_off[phase_blk_off + blk_idx].length_start;
			TwodVertex* col_edge_array = graph_.edge_array_ + edge_offset;
			SortIdx* col_len = graph_.col_len_ + length_start;
			TwodVertex blk_vertex_base = phase_bmp_off * NBPE + blk_idx * BFELL_SORT;

			int c = 0;
			for( ; num_active_rows > 0; ++c) {
				SortIdx next_col_len = col_len[c + 1];
				int i = num_active_rows - 1;
				for( ; i >= 0; --i) {
					SortIdx row = rows[i].sorted;
					TwodVertex src = col_edge_array[row];
					if(shared_visited_[src / NBPE] & (BitmapType(1) << (src % NBPE))) {
						// add to next queue
						int orig = rows[i].orig;
						vertex_enabled[blk_i_start + rows[i].orig_i] = 0;
						--num_enabled;
						if(buf->full()) {
							nq_.push(buf); buf = nq_empty_buffer_.get();
						}
						buf->append_nocheck(src, blk_vertex_base + orig);
						// end this row
						rows[i] = rows[--num_active_rows];
					}
					else if(row >= next_col_len) {
						// end this row
						rows[i] = rows[--num_active_rows];
					}
				}
				col_edge_array += col_len[c];
			}
		}
		tlb->local_size = num_enabled;

		// TODO: calc
		tlb->local_size = 0;

		// make new list to send
		num_enabled = tlb->local_size;
		for(int i = 0; i < phase_size; ++i) {
			if(vertex_enabled[i]) {
				write_list[num_enabled++] = phase_list[i];
			}
		}

		tlb->cur_buffer = buf;
		return num_enabled;
	}

	void bottom_up_gather_nq_size(int* visited_count) {
#if PROFILING_MODE
		profiling::TimeKeeper tk_all;
		MPI_Barrier(mpi.comm_2d);
		fold_competion_wait_ += tk_all;
#endif
#if 1 // which one is faster ?
		int recv_count[mpi.size_2dc]; for(int i = 0; i < mpi.size_2dc; ++i) recv_count[i] = 1;
		MPI_Reduce_scatter(visited_count, &nq_size_, recv_count, MPI_INT, MPI_SUM, mpi.comm_2dr);
		MPI_Allreduce(&nq_size_, &max_nq_size_, 1, MPI_INT, MPI_MAX, mpi.comm_2d);
		int64_t nq_size = nq_size_;
		MPI_Allreduce(&nq_size, &global_nq_size_, 1, MpiTypeOf<int64_t>::type, MPI_SUM, mpi.comm_2d);
#else
		int red_nq_size[mpi.size_2dc];
		struct {
			int nq_size;
			int max_nq_size;
			int64_t global_nq_size;
		} scatter_buffer[mpi.size_2dc], recv_nq_size;
		// gather information within the processor row
		MPI_Reduce(visited_count, red_nq_size, mpi.size_2dc, MPI_INT, MPI_SUM, 0, mpi.comm_2dr);
		if(mpi.rank_2dr == 0) {
			int max_nq_size = 0, sum_nq_size = 0;
			int64_t global_nq_size;
			for(int i = 0; i < mpi.size_2dc; ++i) {
				sum_nq_size += red_nq_size[i];
				if(max_nq_size < red_nq_size[i]) max_nq_size = red_nq_size[i];
			}
			// compute global_nq_size by allreduce within the processor column
			MPI_Allreduce(&sum_nq_size, &global_nq_size, 1, MpiTypeOf<int64_t>::type, MPI_SUM, mpi.comm_2dc);
			for(int i = 0; i < mpi.size_2dc; ++i) {
				scatter_buffer[i].nq_size = red_nq_size[i];
				scatter_buffer[i].max_nq_size = max_nq_size;
				scatter_buffer[i].global_nq_size = global_nq_size;
			}
		}
		// scatter information within the processor row
		MPI_Scatter(scatter_buffer, sizeof(recv_nq_size), MPI_BYTE,
				&recv_nq_size, sizeof(recv_nq_size), MPI_BYTE, 0, mpi.comm_2dr);
		nq_size_ = recv_nq_size.nq_size;
		max_nq_size_ = recv_nq_size.max_nq_size;
		global_nq_size_ = recv_nq_size.global_nq_size;
#endif
#if PROFILING_MODE
		gather_nq_time_ += tk_all;
#endif
	}

#if BF_DEEPER_ASYNC
	class BottomUpComm :  public bfs_detail::BfsAsyncCommumicator::Communicatable {
	public:
		BottomUpComm(ThisType* this__, MPI_Comm mpi_comm__) {
			this_ = this__;
			mpi_comm = mpi_comm__;
			int size_cmask = mpi.size_2dc - 1;
			send_to = (mpi.rank_2dc - 1) & size_cmask;
			recv_from = (mpi.rank_2dc + 1) & size_cmask;
			total_phase = mpi.size_2dc*2;
			comm_phase = 0;
			proc_phase = 0;
			finished = 0;
			for(int i = 0; i < DNBUF; ++i) {
				req[i] = MPI_REQUEST_NULL;
			}
			this_->comm_.input_command(this);
		}
		void advance() {
			int phase = proc_phase++;
			while(comm_phase < phase) sched_yield();
		}
		void finish() {
			while(!finished) sched_yield();
		}
		virtual void comm() {
			int processed_phase = 0;
			int phase = 0;

			set_buffer(-1);
			while(proc_phase <= phase) sched_yield();
			++phase;

			int comp_count = 0;
			while(true) {
				if(processed_phase < proc_phase) {
					set_buffer(processed_phase++);
				}

				assert (processed_phase >= phase);

				MPI_Request* req_ptr = req + (phase & BUFMASK) * 2;
				int index, flag;
				MPI_Status status;
				MPI_Testany(2, req_ptr, &index, &flag, &status);

				// check req_ptr has at least one active handle.
				assert (flag == false || index != MPI_UNDEFINED);

				if(flag == 0 || index == MPI_UNDEFINED) {
					continue;
				}

				assert (index == 0 || index == 1);

				++comp_count;
				if(index) { // recv
					// tell processor completion
					recv_complete(phase, &status);
					comm_phase = phase + 1;
				}
				if(comp_count == 2) {
					// next phase
					if(++phase == total_phase) break;
					while(proc_phase <= phase) sched_yield();
				}
			}
			finished = true;
#if VERVOSE_MODE
			fprintf(IMD_OUT, "bottom-up communication thread finished.\n");
#endif
		}

	protected:
		enum { NBUF = BFS_PARAMS::BOTTOM_UP_BUFFER, DNBUF = NBUF*2, BUFMASK = NBUF-1 };
		ThisType* this_;
		MPI_Comm mpi_comm;
		int send_to, recv_from;
		int total_phase;
		MPI_Request req[DNBUF];
		volatile int comm_phase;
		volatile int proc_phase;
		volatile int finished;

		virtual void recv_complete(int phase, MPI_Status* status) = 0;
		virtual void set_buffer(int phase) = 0;
	};

	class BottomUpBitmapComm :  public BottomUpComm {
	public:
		BottomUpBitmapComm(ThisType* this__, MPI_Comm mpi_comm__, BitmapType** bitmap_buffer__)
			: BottomUpComm(this__, mpi_comm__)
		{
			bitmap_buffer = bitmap_buffer__;
			half_bitmap_width = this_->get_bitmap_size_local() / 2;
			this_->get_visited_pointers(old_vis, 2, this_->old_visited_);
			this_->get_visited_pointers(new_vis, 2, this_->new_visited_);
		}

	protected:
		BitmapType** bitmap_buffer;
		BitmapType* old_vis[2], new_vis[2];
		int half_bitmap_width;

		virtual void recv_complete(int phase, MPI_Status* status) { }

		virtual void set_buffer(int phase) {
			int send_phase = phase;
			int recv_phase = send_phase + 3;
			if(send_phase >= 0) {
				BitmapType* send = bitmap_buffer[send_phase & BUFMASK];
				MPI_Request* req_ptr = req + (phase & BUFMASK) * 2;
				if(send_phase < 2) {
					send = old_vis[send_phase];
				}
				MPI_Isend(send, half_bitmap_width, MpiTypeOf<BitmapType>::type, send_to, 0, mpi_comm, req_ptr);
			}
			if(recv_phase <= total_phase + 1) {
				BitmapType* recv = bitmap_buffer[recv_phase & BUFMASK];
				MPI_Request* req_ptr = req + ((phase + 1) & BUFMASK) * 2 + 1;
				if(recv_phase >= total_phase) {
					recv = new_vis[recv_phase - total_phase];
				}
				MPI_Irecv(recv, half_bitmap_width, MpiTypeOf<BitmapType>::type, recv_from, 0, mpi_comm, req_ptr);
			}
		}
	};

	class BottomUpListComm :  public BottomUpComm {
	public:
		BottomUpListComm(ThisType* this__, MPI_Comm mpi_comm__,
				TwodVertex** list_buffer__, int* list_size__)
			: BottomUpComm(this__, mpi_comm__)
		{
			list_buffer = list_buffer__;
			list_size = list_size__;
			int half_bitmap_width = this_->get_bitmap_size_local() / 2;
			buffer_size = half_bitmap_width * sizeof(BitmapType) / sizeof(TwodVertex);
			this_->get_visited_pointers(old_vis, 2, this_->old_visited_);
			this_->get_visited_pointers(new_vis, 2, this_->new_visited_);
		}

	protected:
		TwodVertex** list_buffer;
		int* list_size;
		TwodVertex* old_vis[2], new_vis[2];
		int buffer_size;

		virtual void recv_complete(int phase, MPI_Status* status) {
			int recv_phase = phase + 3;
			int* size_ptr = &list_size[recv_phase & BUFMASK];
			if(recv_phase >= total_phase) {
				size_ptr = &this_->new_visited_list_size_[recv_phase - total_phase];
			}
			MPI_Get_count(&status[1], MpiTypeOf<TwodVertex>::type, &list_size[recv_phase & BUFMASK]);
		}

		virtual void set_buffer(int phase) {
			int send_phase = phase;
			int recv_phase = send_phase + 4;
			if(send_phase >= 0) {
				TwodVertex* send = list_buffer[send_phase & BUFMASK];
				TwodVertex send_size = list_size[send_phase & BUFMASK];
				MPI_Request* req_ptr = req + (phase & BUFMASK) * 2;
				if(send_phase < 2) {
					send = old_vis[send_phase];
					send_size = this_->old_visited_list_size_[send_phase];
				}
				MPI_Isend(send, send_size, MpiTypeOf<BitmapType>::type, send_to, 0, mpi_comm, req_ptr);
			}
			if(recv_phase <= total_phase + 1) {
				TwodVertex* recv = list_buffer[recv_phase & BUFMASK];
				MPI_Request* req_ptr = req + ((phase + 1) & BUFMASK) * 2 + 1;
				if(recv_phase >= total_phase) {
					recv = new_vis[recv_phase - total_phase];
				}
				MPI_Irecv(recv, buffer_size, MpiTypeOf<BitmapType>::type, recv_from, 0, mpi_comm, req_ptr);
			}
		}
	};

	void bottom_up_search_bitmap() {
		using namespace BFS_PARAMS;
#if PROFILING_MODE
		profiling::TimeKeeper tk_all;
		profiling::TimeKeeper tk_commit;
		profiling::TimeSpan ts_commit;
#endif

		enum { NBUF = BOTTOM_UP_BUFFER, BUFMASK = NBUF-1 };
		int half_bitmap_width = get_bitmap_size_local() / 2;
		assert (cq_buf_size_ >= half_bitmap_width * NBUF);
		BitmapType* bitmap_buffer[NBUF]; get_visited_pointers(bitmap_buffer, NBUF, cq_buf_);
		BitmapType* old_vis[2]; get_visited_pointers(old_vis, 2, old_visited_);
		MPI_Comm mpi_comm = mpi.comm_2dr;
		int comm_size = mpi.size_2dc;
		int comm_rank = mpi.rank_2dc;

		int phase = 0;
		int total_phase = comm_size*2;
		int comm_size_mask = comm_size - 1;
		BottomUpBitmapComm comm(this, mpi_comm, bitmap_buffer);
		int visited_count[comm_size] = {0};

		for(int phase = 0; phase < total_phase; ++phase) {
			BitmapType* phase_bitmap = (phase < 2) ? old_vis[phase] : bitmap_buffer[phase & BUFMASK];
			TwodVertex phase_bmp_off = ((mpi.rank_2dc * 2 + phase) & comm_size_mask) * half_bitmap_width;
			visited_count[(phase/2 + comm_rank) & comm_size_mask] +=
					bottom_up_search_bitmap_process_step(phase_bitmap, phase_bmp_off, half_bitmap_width);
			comm.advance();
		}
		// wait for local_visited is received.
		comm.advance();
		comm.finish();

		bottom_up_gather_nq_size(visited_count);
#if VERVOSE_MODE
		botto_up_print_stt(0, 0, visited_count);
#endif
	}

	int bottom_up_search_list() {
		//
		using namespace BFS_PARAMS;
#if PROFILING_MODE
		profiling::TimeKeeper tk_all;
		profiling::TimeKeeper tk_commit;
		profiling::TimeSpan ts_commit;
#endif

		enum { NBUF = BOTTOM_UP_BUFFER, BUFMASK = NBUF-1 };
		int half_bitmap_width = get_bitmap_size_local() / 2;
		int buffer_size = half_bitmap_width * sizeof(BitmapType) / sizeof(TwodVertex);
		TwodVertex* list_buffer[NBUF]; get_visited_pointers(list_buffer, NBUF, cq_buf_);
		int list_size[NBUF] = {0};
		TwodVertex* old_vis[2]; get_visited_pointers(old_vis, 2, old_visited_);
		int8_t* vertex_enabled = (int8_t*)cache_aligned_xcalloc(buffer_size*sizeof(int8_t));
		MPI_Comm mpi_comm = mpi.comm_2dr;
		int comm_size = mpi.size_2dc;
		int comm_rank = mpi.rank_2dc;

		int phase = 0;
		int total_phase = comm_size*2;
		int comm_size_mask = comm_size - 1;
		BottomUpListComm comm(this, mpi_comm, list_buffer, list_size);
		int visited_count[comm_size] = {0};
#if VERVOSE_MODE
		int64_t num_blocks = 0;
		int64_t num_vertexes = 0;
#endif

		for(int phase = 0; phase - 1 < total_phase; ++phase) {
			int write_phase = phase - 1;
			TwodVertex* phase_list = (phase < 2) ? old_vis[phase] : list_buffer[phase & BUFMASK];
			int phase_size = (phase < 2) ? old_visited_list_size_[phase] : list_size[phase & BUFMASK];
			TwodVertex* write_list = list_buffer[write_phase & BUFMASK];
			TwodVertex phase_bmp_off = ((mpi.rank_2dc * 2 + phase) & comm_size_mask) * half_bitmap_width;
			int write_size = bottom_up_search_list_process_step(
#if VERVOSE_MODE
					num_vertexes, num_blocks,
#endif
					phase_list, phase_size, vertex_enabled, write_list, phase_bmp_off, half_bitmap_width);

			list_size[write_phase & BUFMASK] = write_size;
			visited_count[(phase/2 + comm_rank) & comm_size_mask] += write_size;
			comm.advance();
		}
		// wait for local_visited is received.
		comm.advance();
		comm.finish();

		bottom_up_gather_nq_size(visited_count);
#if VERVOSE_MODE
		botto_up_print_stt(num_blocks, num_vertexes, visited_count);
#endif

		free(vertex_enabled); vertex_enabled = NULL;
		return total_phase;
	}
#else // #if BF_DEEPER_ASYNC
	struct BottomUpBitmapComm :  public bfs_detail::BfsAsyncCommumicator::Communicatable {
		virtual void comm() {
			MPI_Request req[2];
			MPI_Isend(send, half_bitmap_width, MpiTypeOf<BitmapType>::type, send_to, 0, mpi.comm_2dr, &req[0]);
			MPI_Irecv(recv, half_bitmap_width, MpiTypeOf<BitmapType>::type, recv_from, 0, mpi.comm_2dr, &req[1]);
			MPI_Waitall(2, req, NULL);
			complete = 1;
		}
		int send_to, recv_from;
		BitmapType* send, recv;
		int half_bitmap_width;
		volatile int complete;
	};

	void bottom_up_search_bitmap() {
		//
		using namespace BFS_PARAMS;
#if PROFILING_MODE
		profiling::TimeKeeper tk_all;
		profiling::TimeKeeper tk_commit;
		profiling::TimeSpan ts_commit;
#endif

		int half_bitmap_width = get_bitmap_size_local() / 2;
		BitmapType* bitmap_buffer[NBUF];
		for(int i = 0; i < NBUF; ++i) {
			bitmap_buffer[i] = (BitmapType*)cq_buf_ + half_bitmap_width*i;
		}
		memcpy(bitmap_buffer[0], local_visited_, half_bitmap_width*2*sizeof(BitmapType));
		int phase = 0;
		int total_phase = mpi.size_2dc*2;
		int size_cmask = mpi.size_2dc - 1;
		BottomUpBitmapComm comm;
		comm.send_to = (mpi.rank_2dc - 1) & size_cmask;
		comm.recv_from = (mpi.rank_2dc + 1) & size_cmask;
		comm.half_bitmap_width = half_bitmap_width;

		for(int phase = 0; phase - 1 < total_phase; ++phase) {
			int send_phase = phase - 1;
			int recv_phase = phase + 1;

			if(send_phase >= 0) {
				comm.send = bitmap_buffer[send_phase & BUFMASK];
				// send and recv
				if(recv_phase >= total_phase) {
					// recv visited
					int part_idx = recv_phase - total_phase;
					comm.recv = local_visited_ + half_bitmap_width*part_idx;
				}
				else {
					// recv normal
					comm.recv = bitmap_buffer[recv_phase & BUFMASK];
				}
				comm.complete = 0;
				comm_.input_command(&comm);
			}
			if(phase < total_phase) {
				BitmapType* phase_bitmap = bitmap_buffer[phase & BUFMASK];
				TwodVertex phase_bmp_off = ((mpi.rank_2dc * 2 + phase) & size_cmask) * half_bitmap_width;
				bottom_up_search_bitmap_process_step(phase_bitmap, phase_bmp_off, half_bitmap_width);
			}
			if(send_phase >= 0) {
				while(!comm.complete) sched_yield();
			}
		}
#if VERVOSE_MODE
		botto_up_print_stt(0, 0);
#endif
	}
	struct BottomUpListComm :  public bfs_detail::BfsAsyncCommumicator::Communicatable {
		virtual void comm() {
			MPI_Request req[2];
			MPI_Status status[2];
			MPI_Isend(send, send_size, MpiTypeOf<TwodVertex>::type, send_to, 0, mpi.comm_2dr, &req[0]);
			MPI_Irecv(recv, buffer_size, MpiTypeOf<TwodVertex>::type, recv_from, 0, mpi.comm_2dr, &req[1]);
			MPI_Waitall(2, req, status);
			MPI_Get_count(&status[1], MpiTypeOf<TwodVertex>::type, &recv_size);
			complete = 1;
		}
		int send_to, recv_from;
		TwodVertex* send, recv;
		int send_size, recv_size, buffer_size;
		volatile int complete;
	};

	int bottom_up_search_list(int* list_size, TwodVertex* list_buffer) {
		//
		using namespace BFS_PARAMS;
#if PROFILING_MODE
		profiling::TimeKeeper tk_all;
		profiling::TimeKeeper tk_commit;
		profiling::TimeSpan ts_commit;
#endif

		int half_bitmap_width = get_bitmap_size_local() / 2;
		int buffer_size = half_bitmap_width * sizeof(BitmapType) / sizeof(TwodVertex);
	/*	TwodVertex* list_buffer[NBUF];
		for(int i = 0; i < NBUF; ++i) {
			list_buffer[i] = (TwodVertex*)(cq_bitmap_ + half_bitmap_width*i);
		}*/
		int8_t* vertex_enabled = (int8_t*)cache_aligned_xcalloc(buffer_size*sizeof(int8_t));
		int phase = 0;
		int total_phase = mpi.size_2dc*2;
		int size_cmask = mpi.size_2dc - 1;
		BottomUpListComm comm;
		comm.send_to = (mpi.rank_2dc - 1) & size_cmask;
		comm.recv_from = (mpi.rank_2dc + 1) & size_cmask;
		comm.buffer_size = buffer_size;
#if VERVOSE_MODE
		int64_t num_blocks = 0;
		int64_t num_vertexes = 0;
#endif

		for(int phase = 0; phase - 1 < total_phase; ++phase) {
			int send_phase = phase - 2;
			int write_phase = phase - 1;
			int recv_phase = phase + 1;

			if(send_phase >= 0) {
				comm.send_size = list_size[send_phase & BUFMASK];
				comm.send = list_buffer[send_phase & BUFMASK];
				comm.recv = list_buffer[recv_phase & BUFMASK];
				comm.complete = 0;
				comm_.input_command(&comm);
			}
			if(phase < total_phase) {
				TwodVertex* phase_list = list_buffer[phase & BUFMASK];
				TwodVertex* write_list = list_buffer[write_phase & BUFMASK];
				int phase_size = list_size[phase & BUFMASK];
				TwodVertex phase_bmp_off = ((mpi.rank_2dc * 2 + phase) & size_cmask) * half_bitmap_width;
				bottom_up_search_list_process_step(
#if VERVOSE_MODE
						num_vertexes, num_blocks,
#endif
						phase_list, phase_size, vertex_enabled, write_list, phase_bmp_off, half_bitmap_width);
			}
			if(send_phase >= 0) {
				while(!comm.complete) sched_yield();
				list_size[recv_phase & BUFMASK] = comm.recv_size;
			}
		}
#if VERVOSE_MODE
		botto_up_print_stt(num_blocks, num_vertexes);
#endif
		free(vertex_enabled); vertex_enabled = NULL;
		return total_phase;
	}
#endif // #if BF_DEEPER_ASYNC

	void printInformation()
	{
		if(mpi.isMaster() == false) return ;
		using namespace BFS_PARAMS;
		//fprintf(IMD_OUT, "Welcome to Graph500 Benchmark World.\n");
		//fprintf(IMD_OUT, "Check it out! You are running highly optimized BFS implementation.\n");

		fprintf(IMD_OUT, "===== Settings and Parameters. ====\n");
		fprintf(IMD_OUT, "NUM_BFS_ROOTS=%d.\n", NUM_BFS_ROOTS);
		fprintf(IMD_OUT, "max threads=%d.\n", omp_get_max_threads());
		fprintf(IMD_OUT, "sizeof(BitmapType)=%zd.\n", sizeof(BitmapType));
		fprintf(IMD_OUT, "Index Type of Graph: %d bytes per edge.\n", IndexArray::bytes_per_edge);
		fprintf(IMD_OUT, "sizeof(TwodVertex)=%zd.\n", sizeof(TwodVertex));
		fprintf(IMD_OUT, "PACKET_LENGTH=%d.\n", PACKET_LENGTH);
		fprintf(IMD_OUT, "NUM_BFS_ROOTS=%d.\n", NUM_BFS_ROOTS);
		fprintf(IMD_OUT, "NUMBER_PACKING_EDGE_LISTS=%d.\n", NUMBER_PACKING_EDGE_LISTS);
		fprintf(IMD_OUT, "NUMBER_CQ_SUMMARIZING=%d.\n", NUMBER_CQ_SUMMARIZING);
		fprintf(IMD_OUT, "MINIMUN_SIZE_OF_CQ_BITMAP=%d.\n", MINIMUN_SIZE_OF_CQ_BITMAP);
		fprintf(IMD_OUT, "BLOCK_V0_LEGNTH=%d.\n", BLOCK_V0_LEGNTH);
		fprintf(IMD_OUT, "VERVOSE_MODE=%d.\n", VERVOSE_MODE);
		fprintf(IMD_OUT, "SHARED_VISITED_OPT=%d.\n", SHARED_VISITED_OPT);
		fprintf(IMD_OUT, "VALIDATION_LEVEL=%d.\n", VALIDATION_LEVEL);
		fprintf(IMD_OUT, "DENOM_SHARED_VISITED_PART=%d.\n", DENOM_SHARED_VISITED_PART);
		fprintf(IMD_OUT, "BACKWARD_THREASOLD=%d.\n", BACKWARD_THREASOLD);
		fprintf(IMD_OUT, "BACKEARD_DENOMINATOR=%d.\n", BACKEARD_DENOMINATOR);
		fprintf(IMD_OUT, "BFS_BACKWARD=%d.\n", BFS_BACKWARD);
		fprintf(IMD_OUT, "VLQ_COMPRESSION=%d.\n", VLQ_COMPRESSION);
		fprintf(IMD_OUT, "BFS_EXPAND_COMPRESSION=%d.\n", BFS_EXPAND_COMPRESSION);
		fprintf(IMD_OUT, "VERTEX_SORTING=%d.\n", VERTEX_SORTING);
		fprintf(IMD_OUT, "BFS_FORWARD_PREFETCH=%d.\n", BFS_FORWARD_PREFETCH);
		fprintf(IMD_OUT, "BFS_BACKWARD_PREFETCH=%d.\n", BFS_BACKWARD_PREFETCH);
		fprintf(IMD_OUT, "GRAPH_BITVECTOR=%d.\n", GRAPH_BITVECTOR);
		fprintf(IMD_OUT, "GRAPH_BITVECTOR_OFFSET=%d.\n", GRAPH_BITVECTOR_OFFSET);
		fprintf(IMD_OUT, "AVOID_BUSY_WAIT=%d.\n", AVOID_BUSY_WAIT);
		fprintf(IMD_OUT, "SWITCH_FUJI_PROF=%d.\n", SWITCH_FUJI_PROF);
	}

	void prepare_sssp() { }
	void run_sssp(int64_t root, int64_t* pred) { }
	void end_sssp() { }

	// members

	FiberManager fiber_man_;
	bfs_detail::BfsAsyncCommumicator comm_;
	ThreadLocalBuffer** thread_local_buffer_;
	memory::ConcurrentPool<QueuedVertexes> nq_empty_buffer_;
	memory::ConcurrentStack<QueuedVertexes*> nq_;

	TwodVertex* cq_list_;
	TwodVertex cq_size_;
	int nq_size_, max_nq_size_;
	int64_t global_nq_size_;

	// size = local bitmap width
	// TODO: These two buffer is swapped at the beginning of every backward step
	void* new_visited_; // shared memory
	void* old_visited_; // shared memory
	int* new_visited_list_size_; // shared memory
	int* old_visited_list_size_; // shared memory
	bool bitmap_or_list_;

	// size = local bitmap width / 2 * NBUF
	// cq_buf_orig_ + half_bitmap_width_ * NBUF * mpi.rank_z
	void* cq_buf_;
	void* cq_buf_orig_; // shared memory
	void* cq_extra_buf_; // TODO: free extra buffer
	int64_t cq_buf_size_;
	int64_t cq_extra_buf_size_;

	BitmapType* shared_visited_; // shared memory
	TwodVertex* nq_recv_buf_; // shared memory

	int64_t* pred_;

	struct SharedDataSet {
		memory::SpinBarrier sync;
		int *count;
		int *offset; // max(max_threads*2+1, 2*mpi.size_z+1)

		SharedDataSet()
			: sync(mpi.size_z)
			//, th_sync(mpi.size_z * omp_get_max_threads())
		{ }
	} *s_; // shared memory

	struct DynamicDataSet {
		int64_t num_tmp_packets_;
		int64_t tmp_packet_offset_;
		// We count only if CQ is transfered by stream. Otherwise, 0.
		int64_t num_vertices_in_cq_;
		// This is used in backward phase.
		// The number of unvisited vertices in CQ.
		int64_t num_active_vertices_;
		int64_t num_vertices_in_nq_;
		int num_remaining_extract_jobs_;
#if AVOID_BUSY_WAIT
		pthread_mutex_t avoid_busy_wait_sync_;
#endif
#if VERVOSE_MODE
		int64_t num_edge_relax_;
#endif
	} *d_;

	int comm_length_;
	int log_local_bitmap_;
	ExpandCommCommand cq_comm_;
	ExpandCommCommand visited_comm_;

	struct {
		TopDownSender* long_job;
		TopDownSender* short_job;
		TopDownSendEnd* fold_end_job;
		int long_job_length;
		int short_job_length;
	} sched_;

	int current_level_;
	bool forward_or_backward_;

	struct {
		void* thread_local_;
	} buffer_;
#if PROFILING_MODE
	profiling::TimeSpan extract_edge_time_;
	profiling::TimeSpan vertex_scan_time_;
	profiling::TimeSpan core_proc_time_;
	profiling::TimeSpan commit_time_;
	profiling::TimeSpan recv_proc_time_;
	profiling::TimeSpan fold_competion_wait_;
	profiling::TimeSpan gather_nq_time_;
#endif
#if BIT_SCAN_TABLE
	uint8_t bit_scan_table_[BFS_PARAMS::BIT_SCAN_TABLE_SIZE];
#endif
};

template <typename IndexArray, typename TwodVertex, typename PARAMS>
void BfsBase<IndexArray, TwodVertex, PARAMS>::
	run_bfs(int64_t root, int64_t* pred)
{
#if SWITCH_FUJI_PROF
	fapp_start("initialize", 0, 0);
	start_collection("initialize");
#endif
	using namespace BFS_PARAMS;
	pred_ = pred;
	cq_bitmap_ = (BitmapType*)
			page_aligned_xcalloc(sizeof(cq_bitmap_[0])*get_bitmap_size_src());
	shared_visited_ = (BitmapType*)
			page_aligned_xcalloc(sizeof(shared_visited_[0])*get_bitmap_size_tgt());
#if VERVOSE_MODE
	double tmp = MPI_Wtime();
	double start_time = tmp;
	double prev_time = tmp;
	double expand_time = 0.0, fold_time = 0.0, stall_time = 0.0;
	g_fold_send = g_fold_recv = g_bitmap_send = g_bitmap_recv = g_exs_send = g_exs_recv = 0;
#endif
	// threshold of scheduling for extracting CQ.
	const int64_t forward_sched_threshold = get_number_of_local_vertices() * mpi.size_2dr / 1024;
	const int64_t backward_sched_threshold = get_number_of_local_vertices() * mpi.size_2dr / 32;

	const int log_size = get_msb_index(mpi.size_2d);
	const int size_mask = mpi.size_2d - 1;
#define VERTEX_OWNER(v) ((v) & size_mask)
#define VERTEX_LOCAL(v) ((v) >> log_size)

	initialize_memory(pred);
#if VERVOSE_MODE
	if(mpi.isMaster()) fprintf(IMD_OUT, "Time of initialize memory: %f ms\n", (MPI_Wtime() - prev_time) * 1000.0);
	prev_time = MPI_Wtime();
	int64_t actual_traversed_edges = 0;
#endif
	current_level_ = 0;
	forward_or_backward_ = true; // begin with forward search
	int64_t global_visited_vertices = 0;
	// !!root is UNSWIZZLED.!!
	int root_owner = (int)VERTEX_OWNER(root);
	if(root_owner == mpi.rank_2d) {
		int64_t root_local = VERTEX_LOCAL(root);
		pred_[root_local] = root;
#if VERTEX_SORTING
		int64_t sortd_root_local = graph_.vertex_mapping_[root_local];
		int64_t word_idx = sortd_root_local / NUMBER_PACKING_EDGE_LISTS;
		int bit_idx = sortd_root_local % NUMBER_PACKING_EDGE_LISTS;
#else
		int64_t word_idx = root_local / NUMBER_PACKING_EDGE_LISTS;
		int bit_idx = root_local % NUMBER_PACKING_EDGE_LISTS;
#endif
		visited_[word_idx] |= BitmapType(1) << bit_idx;
		expand_root(root_local, &cq_comm_, &visited_comm_);
	}
	else {
		expand_root(-1, &cq_comm_, &visited_comm_);
	}
#if VERVOSE_MODE
	tmp = MPI_Wtime();
	if(mpi.isMaster()) fprintf(IMD_OUT, "Time of first expansion: %f ms\n", (tmp - prev_time) * 1000.0);
	expand_time += tmp - prev_time; prev_time = tmp;
#endif
#undef VERTEX_OWNER
#undef VERTEX_LOCAL

#if SWITCH_FUJI_PROF
	stop_collection("initialize");
	fapp_stop("initialize", 0, 0);
	int extract_kind;
	char *prof_mes[] = { "f_long", "f_short", "b_long", "b_short" };
#endif
	while(true) {
		++current_level_;
#if VERVOSE_MODE
		double level_start_time = MPI_Wtime();
#endif
		d_->num_vertices_in_nq_ = 0;

		fiber_man_.begin_processing();
		comm_.begin_fold_comm(forward_or_backward_);

#if 1 // improved scheduling
		// submit graph extraction job
		if(forward_or_backward_) {
			// forward search
			if(d_->num_vertices_in_cq_ >= forward_sched_threshold) {
				fiber_man_.submit_array(sched_.long_job, sched_.long_job_length, 0);
				d_->num_remaining_extract_jobs_ = sched_.long_job_length;
#if SWITCH_FUJI_PROF
				extract_kind = 0;
#endif
			}
			else {
				fiber_man_.submit_array(sched_.short_job, sched_.short_job_length, 0);
				d_->num_remaining_extract_jobs_ = sched_.short_job_length;
#if SWITCH_FUJI_PROF
				extract_kind = 1;
#endif
			}
		}
		else {
			// backward search
			if(d_->num_active_vertices_ >= backward_sched_threshold) {
				fiber_man_.submit_array(sched_.back_long_job, sched_.long_job_length, 0);
				d_->num_remaining_extract_jobs_ = sched_.long_job_length;
#if SWITCH_FUJI_PROF
				extract_kind = 2;
#endif
			}
			else {
				fiber_man_.submit_array(sched_.back_short_job, sched_.short_job_length, 0);
				d_->num_remaining_extract_jobs_ = sched_.short_job_length;
#if SWITCH_FUJI_PROF
				extract_kind = 3;
#endif
			}
		}
#else
		// submit graph extraction job
		if(d_->num_vertices_in_cq_ >= sched_threshold) {
			if(forward_or_backward_)
			fiber_man_.submit_array(sched_.long_job, sched_.long_job_length, 0);
			else
				fiber_man_.submit_array(sched_.back_long_job, sched_.long_job_length, 0);

			d_->num_remaining_extract_jobs_ = sched_.long_job_length;
		}
		else {
			if(forward_or_backward_)
			fiber_man_.submit_array(sched_.short_job, sched_.short_job_length, 0);
			else
				fiber_man_.submit_array(sched_.back_short_job, sched_.short_job_length, 0);

			d_->num_remaining_extract_jobs_ = sched_.short_job_length;
		}
#endif
#if SWITCH_FUJI_PROF
		fapp_start(prof_mes[extract_kind], 0, 0);
		start_collection(prof_mes[extract_kind]);
#endif
#if VERVOSE_MODE
		if(mpi.isMaster()) {
			double nq_percent = (double)d_->num_vertices_in_cq_ / (get_actual_number_of_local_vertices() * mpi.size_2dr) * 100.0;
			double visited_percent = (double)global_visited_vertices / get_actual_number_of_global_vertices() * 100.0;
			fprintf(IMD_OUT, "Level %d (%s) start, # of tasks : %d\n", current_level_,
					forward_or_backward_ ? "Forward" : "Backward", d_->num_remaining_extract_jobs_);
			fprintf(IMD_OUT, "NQ: %f %%, Visited: %f %%", nq_percent, visited_percent);
			if(forward_or_backward_)
				fprintf(IMD_OUT, "\n");
			else
				fprintf(IMD_OUT, ", Active: %f %%\n",
						(double)d_->num_active_vertices_ / (get_actual_number_of_local_vertices() * mpi.size_2dc) * 100.0);
		}
		d_->num_edge_relax_ = 0;
#endif
		d_->num_active_vertices_ = 0;
#if PROFILING_MODE
		fiber_man_.reset_wait_time();
#endif

#pragma omp parallel
		fiber_man_.enter_processing();

		assert(comm_.check_num_send_buffer());
#if VERVOSE_MODE
		tmp = MPI_Wtime(); fold_time += tmp - prev_time; prev_time = tmp;
#endif
#if SWITCH_FUJI_PROF
		stop_collection(prof_mes[extract_kind]);
		fapp_stop(prof_mes[extract_kind], 0, 0);
		fapp_start("sync", 0, 0);
		start_collection("sync");
#endif
		int64_t num_nq_vertices = d_->num_vertices_in_nq_;

		int64_t global_nq_vertices;
		MPI_Allreduce(&num_nq_vertices, &global_nq_vertices, 1,
				get_mpi_type(num_nq_vertices), MPI_SUM, mpi.comm_2d);
		global_visited_vertices += global_nq_vertices;
#if VERVOSE_MODE
		tmp = MPI_Wtime(); stall_time += tmp - prev_time; prev_time = tmp;
		actual_traversed_edges += d_->num_edge_relax_;
#endif
#if PROFILING_MODE
		if(forward_or_backward_)
			extract_edge_time_.submit("forward edge", current_level_);
		else
			extract_edge_time_.submit("backward edge", current_level_);

		commit_time_.submit("extract commit", current_level_);
		vertex_scan_time_.submit("vertex scan time", current_level_);
#if DETAILED_PROF_MODE
		core_proc_time_.submit("core processing time", current_level_);
#endif
		recv_proc_time_.submit("recv proc", current_level_);
		comm_.submit_prof_info("send comm wait", current_level_);
		fiber_man_.submit_wait_time("fiber man wait", current_level_);
		profiling::g_pis.submitCounter(d_->num_edge_relax_, "edge relax", current_level_);
#endif
#if DEBUG_PRINT
	if(mpi.isMaster()) printf("global_nq_vertices=%"PRId64"\n", global_nq_vertices);
#endif
#if SWITCH_FUJI_PROF
		stop_collection("sync");
		fapp_stop("sync", 0, 0);
#endif
		if(global_nq_vertices == 0)
			break;
#if SWITCH_FUJI_PROF
		fapp_start("expand", 0, 0);
		start_collection("expand");
#endif
		expand(global_nq_vertices, global_visited_vertices, &cq_comm_, &visited_comm_);
#if SWITCH_FUJI_PROF
		stop_collection("expand");
		fapp_stop("expand", 0, 0);
#endif
#if VERVOSE_MODE
		tmp = MPI_Wtime();
		double time_of_level = MPI_Wtime() - level_start_time;
		if(mpi.isMaster()) {
			fprintf(IMD_OUT, "Edge relax: %f M/s (%"PRId64"), Time: %f ms\n",
					d_->num_edge_relax_ / 1000000.0 / time_of_level, d_->num_edge_relax_, time_of_level * 1000.0);
		}
		expand_time += tmp - prev_time; prev_time = tmp;
#endif
	}
#if PROFILING_MODE
	comm_.submit_mem_info();
#endif
#if VERVOSE_MODE
	if(mpi.isMaster()) fprintf(IMD_OUT, "Time of BFS: %f ms\n", (MPI_Wtime() - start_time) * 1000.0);
	double time3[3] = { fold_time, expand_time, stall_time };
	double timesum3[3];
	int64_t commd[7] = { g_fold_send, g_fold_recv, g_bitmap_send, g_bitmap_recv, g_exs_send, g_exs_recv, actual_traversed_edges };
	int64_t commdsum[7];
	MPI_Reduce(time3, timesum3, 3, MPI_DOUBLE, MPI_SUM, 0, mpi.comm_2d);
	MPI_Reduce(commd, commdsum, 7, get_mpi_type(commd[0]), MPI_SUM, 0, mpi.comm_2d);
	if(mpi.isMaster()) {
		fprintf(IMD_OUT, "Avg time of fold: %f ms\n", timesum3[0] / mpi.size_2d * 1000.0);
		fprintf(IMD_OUT, "Avg time of expand: %f ms\n", timesum3[1] / mpi.size_2d * 1000.0);
		fprintf(IMD_OUT, "Avg time of stall: %f ms\n", timesum3[2] / mpi.size_2d * 1000.0);
		fprintf(IMD_OUT, "Avg fold_send: %"PRId64"\n", commdsum[0] / mpi.size_2d);
		fprintf(IMD_OUT, "Avg fold_recv: %"PRId64"\n", commdsum[1] / mpi.size_2d);
		fprintf(IMD_OUT, "Avg bitmap_send: %"PRId64"\n", commdsum[2] / mpi.size_2d);
		fprintf(IMD_OUT, "Avg bitmap_recv: %"PRId64"\n", commdsum[3] / mpi.size_2d);
		fprintf(IMD_OUT, "Avg exs_send: %"PRId64"\n", commdsum[4] / mpi.size_2d);
		fprintf(IMD_OUT, "Avg exs_recv: %"PRId64"\n", commdsum[5] / mpi.size_2d);
		fprintf(IMD_OUT, "Actual traversed edges: %"PRId64" (%f %%)\n", commdsum[6],
				(double)commdsum[6] / graph_.num_global_edges_ * 100.0);
	}
#endif
	free(cq_bitmap_); cq_bitmap_ = NULL;
	free(shared_visited_); shared_visited_ = NULL;
}

#endif /* BFS_HPP_ */
