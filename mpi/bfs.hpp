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

class AsyncCommBuffer {
public:
	AsyncCommBuffer() : length_(0) { }
	virtual ~AsyncCommBuffer() { }
	virtual void add(void* ptr__, int offset, int length) = 0;
	int length_;
};

struct AlltoAllCommSetting {
	int comm_size;
	int buffer_size;
	int send_queue_limit;
};

class AlltoAllHandler {
public:
	virtual ~AlltoAllHandler() { }
	virtual void get_setting(AlltoAllCommSetting* s) = 0;
	virtual AsyncCommBuffer* alloc_buffer() = 0;
	virtual void free_buffer(AsyncCommBuffer* buf) = 0;
	virtual void set_buffer(bool send_or_recv, AsyncCommBuffer* buf, int target, MPI_Request* req) = 0;
	virtual int recv_status(AsyncCommBuffer* buf, MPI_Status* stt) = 0;
	virtual void notify_completion(bool send_or_recv, AsyncCommBuffer* buf, int target) = 0;
	virtual void finished() = 0;
};

class AsyncCommHandler {
public:
	virtual ~AsyncCommHandler() { }
	virtual void probe() = 0;
};

class CommCommand {
public:
	virtual ~CommCommand() { }
	virtual void comm_cmd() = 0;
};

class AsyncCommManager
{
	struct CommTarget {
		pthread_mutex_t send_mutex;
		// monitor : send_mutex
		volatile int reserved_size_;
		volatile int filled_size_;
		AsyncCommBuffer* next_buf;
		AsyncCommBuffer* cur_buf;
		// monitor : thread_sync_
		std::deque<AsyncCommBuffer*> send_queue;
		AsyncCommBuffer* send_buf;
		AsyncCommBuffer* recv_buf;
	};
public:
	AsyncCommManager(FiberManager* fiber_man__)
		: fiber_man_(fiber_man__)
	{
		cuda_enabled_ = false;
	}
	void init(AlltoAllHandler** hdls, int count) {
		d_ = new DynamicDataSet();
		pthread_mutex_init(&d_->thread_sync_, NULL);
		pthread_cond_init(&d_->thread_state_,  NULL);
		d_->cleanup_ = false;
		d_->command_active_ = false;
		d_->suspended_ = false;
		d_->terminated_ = false;

		node_list_length_ = 0;
		for(int i = 0; i < count; ++i) {
			handlers_.push_back(hdls[i]);
			if(node_list_length_ < hdls[i]->comm_size())
				node_list_length_ = hdls[i]->comm_size();
		}
		active_ = NULL;

		node_ = new CommTarget[node_list_length_]();
		mpi_reqs_ = (MPI_Request*)malloc(sizeof(mpi_reqs_[0])*node_list_length_*REQ_TOTAL);

		d_->total_send_queue_ = 0;
		for(int i = 0; i < node_list_length_; ++i) {
			pthread_mutex_init(&node_[i].send_mutex, NULL);
			node_[i].cur_buf = NULL;
			node_[i].send_buf = NULL;
			node_[i].recv_buf = NULL;
			for(int k = 0; k < REQ_TOTAL; ++k) {
				mpi_reqs_[REQ_TOTAL*i + k] = MPI_REQUEST_NULL;
			}
		}

		pthread_create(&d_->thread_, NULL, comm_thread_routine_, this);
	}

	virtual ~AsyncCommManager()
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

			for(int i = 0; i < node_list_length_; ++i) {
				pthread_mutex_destroy(&node_[i].send_mutex);
			}

#if CUDA_ENABLED
			if(cuda_enabled_) {
				CudaStreamManager::begin_cuda();
				CUDA_CHECK(cudaHostUnregister(buffer_.fold_));
				CudaStreamManager::end_cuda();
			}
#endif

			delete [] node_; node_ = NULL;
			free(mpi_reqs_); mpi_reqs_ = NULL;

			delete d_; d_ = NULL;
		}
	}

	void begin_comm(int handler_idx)
	{
#if !BFS_BACKWARD
		assert (forward_or_backward);
#endif
		CommCommand cmd;
		cmd.kind = SEND_START;
		assert (active_ == NULL);
		active_ = handlers_[handler_idx];
		active_->get_setting(&s_);

		for(int i = 0; i < s_.comm_size; ++i) {
			CommTarget& node = node_[i];
			node.reserved_size_ = node.filled_size_ = s_.buffer_size;
			assert (node.next_buf == NULL);
			assert (node.cur_buf == NULL);
		}

		put_command(cmd);
	}

	/**
	 * Asynchronous send.
	 * When the communicator receive data, it will call fold_received(FoldCommBuffer*) function.
	 * To reduce the memory consumption, when the communicator detects stacked jobs,
	 * it also process the tasks in the fiber_man_ except the tasks that have the lowest priority (0).
	 * This feature realize the fixed memory consumption.
	 */
	template <bool proc>
	void send(void* ptr, int length, int target)
	{
		assert(length > 0);
		CommTarget& node = node_[target];
		bool process_task = false;

//#if ASYNC_COMM_LOCK_FREE
		do {
			int offset = __sync_fetch_and_add(&node.reserved_size_, length);
			if(offset >= s_.buffer_size) {
				// wait
				int count = 0;
				while(node.reserved_size_ >= s_.buffer_size) {
					if(proc && count++ >= 1000) {
						while(fiber_man_->process_task(1)); // process recv task
					}
				}
				continue ;
			}
			else if(offset + length >= s_.buffer_size) {
				// swap buffer
				while(offset != node.filled_size_) ;
				if(node.cur_buf != NULL) {
					send_submit(target);
				}
				else {
					node.cur_buf = get_send_buffer<proc>(); // Maybe, this takes much time.
				}
				node.next_buf = get_send_buffer<proc>(); // Maybe, this takes much time.
				// This order is important.
				node.filled_size_ = 0;
				__sync_synchronize(); // membar
				node.reserved_size_ = length;
				process_task = true;
			}
			node.cur_buf->add(ptr, offset, length);
			__sync_fetch_and_add(&node.filled_size_, length);
			break;
		} while(true);

		if(proc && process_task) {
			while(fiber_man_->process_task(1)); // process recv task
		}
// #endif
	}

	void send_end(int target)
	{
		CommTarget& node = node_[target];
		assert (node.reserved_size_ == node.filled_size_);
		if(node.filled_size_ > 0) {
			send_submit(target);
		}
		send_submit(target);
	}

	void input_command(CommCommand* comm)
	{
		CommCommand cmd;
		cmd.kind = MANUAL_CMD;
		cmd.cmd = comm;
		put_command(cmd);
	}

	void register_handler(AsyncCommHandler* comm)
	{
		CommCommand cmd;
		cmd.kind = ADD_HANDLER;
		cmd.handler = comm;
		put_command(cmd);
	}

	void remove_handler(AsyncCommHandler* comm)
	{
		CommCommand cmd;
		cmd.kind = REMOVE_HANDLER;
		cmd.handler = comm;
		put_command(cmd);
	}

#if PROFILING_MODE
	void submit_prof_info(const char* str, int number) {
		comm_time_.submit(str, number);
	}
#endif
#ifndef NDEBUG
	bool check_num_send_buffer() { return (d_->total_send_queue_ == 0); }
#endif
private:

	enum COMM_COMMAND {
		SEND_START,
		SEND,
		MANUAL_CMD,
		ADD_HANDLER,
		REMOVE_HANDLER,
	};

	struct CommCommand {
		COMM_COMMAND kind;
		union {
			// SEND
			int target;
			// COMM_COMMAND
			CommCommand* cmd;
			AsyncCommHandler* handler;
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

		// accessed by comm thread only
		int total_send_queue_;
	} *d_;

	enum MPI_REQ_INDEX {
		REQ_SEND = 0,
		REQ_RECV = 1,
		REQ_TOTAL = 2,
	};

	std::vector<AlltoAllHandler*> handlers_;
	std::vector<AsyncCommHandler*> async_comm_handlers_;
	AlltoAllHandler* active_;

	int node_list_length_;
	AlltoAllCommSetting s_;
	CommTarget* node_;
	FiberManager* fiber_man_;

	bool cuda_enabled_;

	// node list where we need to set receive buffer
	std::deque<int> recv_stv;

	// accessed by communication thread only
	MPI_Request* mpi_reqs_;

#if PROFILING_MODE
	profiling::TimeSpan comm_time_;
#endif

	static void* comm_thread_routine_(void* pThis) {
		static_cast<AsyncCommManager*>(pThis)->comm_thread_routine();
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
						for(int i = 0; i < s_.comm_size; ++i) {
							AsyncCommBuffer* rb = active_->alloc_buffer();
							assert (rb != NULL);
							active_->set_buffer(false, rb, i, &mpi_reqs_[REQ_TOTAL*i + REQ_RECV]);
						}
						num_send_active = num_recv_active = s_.comm_size;
						break;
					case SEND:
						set_send_buffer(cmd.target);
						break;
					case MANUAL_CMD:
						cmd.cmd->comm_cmd();
						break;
					case ADD_HANDLER:
						async_comm_handlers_.push_back(cmd.handler);
						break;
					case REMOVE_HANDLER:
						for(std::vector<AsyncCommHandler*>::iterator it = async_comm_handlers_.begin();
								it != async_comm_handlers_.size(); ++it)
						{
							if(*it == cmd.handler) {
								async_comm_handlers_.erase(it);
								break;
							}
						}
						break;
					}
					pthread_mutex_lock(&d_->thread_sync_);
				}
				pthread_mutex_unlock(&d_->thread_sync_);
			}
			if(num_recv_active == 0 && num_send_active == 0 && async_comm_handlers_.size() == 0) {
				pthread_mutex_lock(&d_->thread_sync_);
				if(d_->command_active_ == false) {
					d_->suspended_ = true;
					if( d_->terminated_ ) { pthread_mutex_unlock(&d_->thread_sync_); break; }
					pthread_cond_wait(&d_->thread_state_, &d_->thread_sync_);
				}
				pthread_mutex_unlock(&d_->thread_sync_);
			}

			if(num_recv_active != 0 || num_send_active != 0) {
				int index;
				int flag;
				MPI_Status status;
				MPI_Testany(s_.comm_size * (int)REQ_TOTAL, mpi_reqs_, &index, &flag, &status);

				if(flag == 0 || index == MPI_UNDEFINED) {
					continue;
				}

				const int src_c = index/REQ_TOTAL;
				const MPI_REQ_INDEX req_kind = (MPI_REQ_INDEX)(index%REQ_TOTAL);
				const bool b_send = (req_kind == REQ_SEND);

				CommTarget& comm_node = node_[src_c];
				AsyncCommBuffer* buf = b_send ? comm_node.send_buf : comm_node.recv_buf;

				assert (mpi_reqs_[index] == MPI_REQUEST_NULL);
				mpi_reqs_[index] = MPI_REQUEST_NULL;

				if(req_kind == REQ_RECV) {
					active_->recv_status(buf, &status);
				}

				bool completion_message = (buf->length_ == 0);
				// complete
				if(b_send) {
					// send buffer
					comm_node.send_buf = NULL;
					active_->free_buffer(buf);
					if(completion_message) {
						// sent fold completion
						--num_send_active;
						if(num_recv_active == 0 && num_send_active == 0) {
							active_->finished();
						}
					}
					else {
						active_->notify_completion(true, buf, src_c);
						set_send_buffer(src_c);
					}
				}
				else {
					// recv buffer
					if(completion_message) {
						// received fold completion
						--num_recv_active;
						comm_node.recv_buf = NULL;
						active_->free_buffer(buf);
						if(num_recv_active == 0 && num_send_active == 0) {
							active_->finished();
						}
					}
					else {
						// set new buffer for next receiving
						recv_stv.push_back(src_c);

						active_->notify_completion(false, buf, src_c);
					}
				}

				// process recv starves
				while(recv_stv.size() > 0) {
					int target = recv_stv.front();
					AsyncCommBuffer* rb = active_->alloc_buffer();
					if(rb == NULL) break;
					active_->set_buffer(false, rb, target, &mpi_reqs_[REQ_TOTAL*target + REQ_RECV]);
					recv_stv.pop_front();
				}
			}

			for(int i = 0; i < async_comm_handlers_.size(); ++i) {
				async_comm_handlers_[i]->probe();
			}
		}
	}

	template <bool proc>
	AsyncCommBuffer* get_send_buffer() {
#if PROFILING_MODE
		profiling::TimeKeeper tk_wait;
		profiling::TimeSpan ts_proc;
#endif
		while(d_->total_send_queue_ > s_.comm_size * s_.send_queue_limit) if(proc) {
#if PROFILING_MODE
			profiling::TimeKeeper tk_proc;
#endif
			while(fiber_man_->process_task(1)) // process recv task
#if PROFILING_MODE
			{ ts_proc += tk_proc; break; }
#else
				;
#endif
		}
#if PROFILING_MODE
		comm_time_ += profiling::TimeSpan(tk_wait) - ts_proc;
#endif
		return active_->alloc_buffer();
	}

	AsyncCommBuffer* get_recv_buffer() {
		return active_->alloc_buffer();
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

	void set_send_buffer(int target)
	{
		CommTarget& node = node_[target];
		AsyncCommBuffer* sb = NULL;
		if(node.send_buf) return ;
		pthread_mutex_lock(&d_->thread_sync_);
		if(node.send_queue.size() > 0) {
			sb = node.send_queue.front(); node.send_queue.pop_front(); --d_->total_send_queue_;
		}
		pthread_mutex_unlock(&d_->thread_sync_);
		if(sb) active_->set_buffer(true, sb, target, &mpi_reqs_[REQ_TOTAL*target + REQ_SEND]);
	}

	void send_submit(int target)
	{
		CommTarget& node = node_[target];
		node.cur_buf->length_ = node.filled_size_;

		CommCommand cmd;
		cmd.kind = SEND;
		cmd.target = target;
		bool command_active;

		pthread_mutex_lock(&d_->thread_sync_);
		node.send_queue.push_back(node.cur_buf); ++d_->total_send_queue_;
		d_->command_queue_.push_back(cmd);
		command_active = d_->command_active_;
		if(command_active == false) d_->command_active_ = true;
		pthread_mutex_unlock(&d_->thread_sync_);

		if(command_active == false) pthread_cond_broadcast(&d_->thread_state_);

		node.cur_buf = node.next_buf;
		node.next_buf = NULL;
	}

	void send_submit__(int target)
	{
		CommTarget& dest_node = node_[target];
		AsyncCommBuffer* sb = dest_node.cur_buf;

		CommCommand cmd;
		cmd.kind = SEND;
		cmd.target = target;
		bool command_active;

		pthread_mutex_lock(&d_->thread_sync_);
		dest_node.send_queue.push_back(sb);
		d_->command_queue_.push_back(cmd);
		command_active = d_->command_active_;
		if(command_active == false) d_->command_active_ = true;
		pthread_mutex_unlock(&d_->thread_sync_);

		if(command_active == false) pthread_cond_broadcast(&d_->thread_state_);
	}
};

template <typename ELEM>
class BFSCommBufferImpl : public AsyncCommBuffer {
public:
	enum { BUF_SIZE = BFS_PARAMS::COMM_BUFFER_SIZE / sizeof(ELEM) };
	BFSCommBufferImpl(void* buffer__, void* obj_ptr__)
		: buffer_((ELEM*)buffer__)
		, obj_ptr_(obj_ptr__)
	{ }
	virtual ~BFSCommBufferImpl() { }
	virtual void add(void* ptr__, int offset, int length) {
		memcpy(buffer_ + offset, ptr__, length*sizeof(ELEM));
	}

	ELEM* buffer_;
	// info
	int target_; // rank to which send or from which receive
	void* obj_ptr_;
};



template <typename TwodVertex, typename PARAMS>
class BfsBase
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

		// non-parameters
		NBPE = sizeof(BitmapType)*8,

		TOP_DOWN_COMM_H = 0,
		BOTTOM_UP_COMM_H = 1,
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

	struct LocalPacket {
		enum {
			TOP_DOWN_LENGTH = BFS_PARAMS::PACKET_LENGTH/sizeof(uint32_t),
			BOTTOM_UP_LENGTH = BFS_PARAMS::PACKET_LENGTH/sizeof(TwodVertex)
		};
		int length;
		int64_t src;
		union {
			uint32_t t[TOP_DOWN_LENGTH];
			TwodVertex b[BOTTOM_UP_LENGTH];
		} data;
	};

	struct ThreadLocalBuffer {
		QueuedVertexes* cur_buffer; // TODO: initialize
		LocalPacket fold_packet[1];
	};

	BfsBase(bool cuda_enabled)
		: comm_(this, cuda_enabled, &fiber_man_)
#if 0
		, recv_task_(65536)
#endif
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

	template <typename T>
	void get_shared_mem_pointer(void*& ptr, int64_t width, T** local_ptr, T** orig_ptr) {
		if(orig_ptr) orig_ptr = (T*)ptr;
		if(local_ptr) local_ptr = (T*)ptr + width*mpi.rank_z;
		ptr = (uint8_t*)ptr + width*sizeof(T)*mpi.size_z;
	}

	void allocate_memory(Graph2DCSR<TwodVertex>& g)
	{
		const int max_threads = omp_get_max_threads();
		const int max_comm_size = std::max(mpi.size_2dc, mpi.size_2dr);

		/**
		 * Buffers for computing BFS
		 * - next queue: This is vertex list in the top-down phase, and <pred, target> tuple list in the bottom-up phase.
		 * - thread local buffer (includes local packet)
		 * - two visited memory (for double buffering)
		 * - working memory: This is used
		 * 	1) to store steaming visited in the bottom-up search phase
		 * 		required size: half_bitmap_width * BOTTOM_UP_BUFFER (for each place)
		 * 	2) to store next queue vertices in the bottom-up expand phase
		 * 		required size: half_bitmap_width * 2 (for each place)
		 * - shared visited:
		 * - shared visited update (to store the information to update shared visited)
		 * - current queue extra memory (is allocated dynamically when the it is required)
		 * - communication buffer for asynchronous communication:
		 */

		thread_local_buffer_ = (ThreadLocalBuffer**)malloc(sizeof(thread_local_buffer_[0])*max_threads);
		d_ = (DynamicDataSet*)malloc(sizeof(d_[0]));

		const int buffer_width = roundup<CACHE_LINE>(
				sizeof(ThreadLocalBuffer) + sizeof(LocalPacket) * max_comm_size);
		buffer_.thread_local_ = cache_aligned_xcalloc(buffer_width*max_threads);
		for(int i = 0; i < max_threads; ++i) {
			ThreadLocalBuffer* tlb = (ThreadLocalBuffer*)
							((uint8_t*)buffer_.thread_local_ + buffer_width*i);
			tlb->cur_buffer = NULL;
			thread_local_buffer_[i] = tlb;
		}

		int64_t bitmap_width = get_bitmap_size_local();
		int64_t half_bitmap_width = bitmap_width / 2;
		work_buf_size_ = half_bitmap_width * BFS_PARAMS::BOTTOM_UP_BUFFER * sizeof(BitmapType);
		int shared_offset_length = (max_threads * mpi.size_z * 2 + 1);
		int64_t total_size_of_shared_memory =
				bitmap_width * 2 * sizeof(BitmapType) * mpi.size_z + // new and old visited
				work_buf_size_ * mpi.size_z + // work_buf_
				bitmap_width * sizeof(BitmapType) * mpi.size_2dr * 2 + // two shared visited memory
				sizeof(SharedDataSet) + sizeof(int) * shared_offset_length;
#if VERVOSE_MODE
		if(mpi.isMaster()) fprintf(IMD_OUT, "Allocating shared memory: %f GB per node.\n", to_giga(total_size_of_shared_memory));
#endif

		void smem_ptr = buffer_.shared_memory_ = shared_malloc(mpi.comm_z, total_size_of_shared_memory);

		get_shared_mem_pointer(smem_ptr, bitmap_width, (BitmapType**)&new_visited_, NULL);
		get_shared_mem_pointer(smem_ptr, bitmap_width, (BitmapType**)&old_visited_, NULL);
		get_shared_mem_pointer(smem_ptr, half_bitmap_width * BFS_PARAMS::BOTTOM_UP_BUFFER, (BitmapType**)&work_buf_, (BitmapType**)&work_extra_buf_);
		get_shared_mem_pointer(smem_ptr, bitmap_width * mpi.size_2dr, NULL, (BitmapType**)&shared_visited_);
		get_shared_mem_pointer(smem_ptr, bitmap_width * mpi.size_2dr, NULL, (BitmapType**)&nq_recv_buf_);
		s_ = new (smem_ptr) SharedDataSet(); smem_ptr = (uint8_t*)smem_ptr + sizeof(SharedDataSet);
		s_->offset = (int*)smem_ptr; smem_ptr = (uint8_t*)smem_ptr + sizeof(int)*shared_offset_length;

		assert (smem_ptr == buffer_.shared_memory_ + shared_offset_length);

		cq_list_ = NULL;
		global_nq_size_ = max_nq_size_ = nq_size_ = cq_size_ = 0;
		bitmap_or_list_ = false;
		work_extra_buf_ = NULL;
		work_extra_buf_size_ = 0;
	}

	void deallocate_memory()
	{
		free(buffer_.thread_local_); buffer_.thread_local_ = NULL;
		shared_free(buffer_.shared_memory_); buffer_.shared_memory_ = NULL;
		free(d_); d_ = NULL;
		free(thread_local_buffer_); thread_local_buffer_ = NULL;
	}

	void initialize_memory(int64_t* pred)
	{
		using namespace BFS_PARAMS;
		int64_t num_local_vertices = int64_t(1) << graph_.log_actual_global_verts_;
		int64_t bitmap_width = get_bitmap_size_local();
		int64_t shared_vis_width = bitmap_width * sizeof(BitmapType) * mpi.size_2dr;

		BitmapType* visited = new_visited_;
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
			for(int64_t i = 0; i < bitmap_width; ++i) {
				visited[i] = 0;
			}
			// clear shared visited
#pragma omp for nowait
			for(int64_t i = 0; i < new_visited_; ++i) {
				shared_visited[i] = 0;
			}

			// clear fold packet buffer
			//LocalPacket* packet_array = thread_local_buffer_[omp_get_thread_num()]->fold_packet;
			//for(int i = 0; i < comm_length_; ++i) {
			//	packet_array[i].num_edges = 0;
			//}
		}
	}

	//-------------------------------------------------------------//
	// Async communication
	//-------------------------------------------------------------//

	struct BFSCommBufferData {
		uint8_t mem[BFS_PARAMS::COMM_BUFFER_SIZE];
		BFSCommBufferImpl<uint32_t> top_down_buffer;
		BFSCommBufferImpl<TwodVertex> bottom_up_buffer;

		BFSCommBufferData()
			: top_down_buffer(mem, this)
			, bottom_up_buffer(mem, this)
		{ }
	};

	class CommBufferPool : public memory::ConcurrentPool<BFSCommBufferData> {
	public:
		CommBufferPool()
			: memory::ConcurrentPool<BFSCommBufferData>()
		{
			num_extra_buffer_ = 0;
		}

		void lock() {
			pthread_mutex_lock(&thread_sync_);
		}
		void unlock() {
			pthread_mutex_unlock(&thread_sync_);
		}

		int num_extra_buffer_;
	protected:
		virtual BFSCommBufferData* allocate_new() {
#if PROFILING_MODE
			__sync_fetch_and_add(&num_extra_buffer_, 1);
#endif
			return new (page_aligned_xmalloc(sizeof(BFSCommBufferData))) BFSCommBufferData();
		}
	};

	template <typename T>
	class CommHandlerBase : public AlltoAllHandler {
		typedef BFSCommBufferImpl<T> BufferType;
	public:
		CommHandlerBase(ThisType* this__, MPI_Comm comm__, int comm_size__, int tag__)
			: this_(this__)
			, pool_(&this__->comm_buffer_pool_)
			, comm_(comm__)
			, comm_size_(comm_size__)
			, tag_(tag__)
		{ }
		virtual ~CommHandlerBase() { }
		virtual void free_buffer(AsyncCommBuffer* buf__) {
			BFSCommBufferImpl<T>* buf_ = static_cast<BFSCommBufferImpl<T>*>(buf__);
			buf_->length_ = 0;
			BFSCommBufferData* buf = static_cast<BFSCommBufferData*>(buf_->obj_ptr_);
			pool_->free(buf);
		}
		virtual void get_setting(AlltoAllCommSetting* s) {
			s->comm_size = comm_size_;
			s->buffer_size = BufferType::BUF_SIZE;
			s->send_queue_limit = BFS_PARAMS::SEND_BUFFER_LIMIT;
		}
		virtual void set_buffer(bool send_or_recv, AsyncCommBuffer* buf_, int target, MPI_Request* req) {
			BufferType* buf = static_cast<BufferType*>(buf_);
			if(send_or_recv) {
				MPI_Isend(buf->buffer_, buf->length_, MpiTypeOf<T>::type,
						target, tag_, comm_, req);
			}
			else {
				MPI_Irecv(buf->buffer_, BufferType::BUF_SIZE, MpiTypeOf<T>::type,
						target, tag_, comm_, req);
			}
		}
		virtual int recv_status(AsyncCommBuffer* buf_, MPI_Status* stt) {
			BufferType* buf = static_cast<BufferType*>(buf_);
			MPI_Get_count(stt, MpiTypeOf<T>::type, &buf->length_);
			return buf->length_;
		}
		virtual void finished() {
			this_->fiber_man_.end_processing();
		}

	private:
		ThisType* this_;
		CommBufferPool* pool_;
		MPI_Comm comm_;
		int comm_size_;
		int tag_;
	};

	class TopDownCommHandler : public CommHandlerBase<uint32_t> {
	public:
		TopDownCommHandler(ThisType* this__)
			: CommHandlerBase<uint32_t>(this__,
					mpi.comm_2dc, mpi.size_2dr, BFS_PARAMS::TOP_DOWN_FOLD_TAG)
			  { }

		virtual AsyncCommBuffer* alloc_buffer() {
			return &this->pool_->get()->top_down_buffer;
		}
		virtual void notify_completion(bool send_or_recv, AsyncCommBuffer* buf_, int target) {
			if(send_or_recv == false) {
				BFSCommBufferImpl<uint32_t>* buf = static_cast<BFSCommBufferImpl<uint32_t>*>(buf_);
				buf->target_ = target;
#if VERVOSE_MODE
				g_tp_comm += buf->length_ * sizeof(uint32_t);
#endif
				this->this_->fiber_man_.submit(new TopDownReceiver(this->this_, buf), 1);
			}
		}
	};

	class BottomUpCommHandler : public CommHandlerBase<TwodVertex> {
	public:
		BottomUpCommHandler(ThisType* this__)
			: CommHandlerBase<TwodVertex>(this__,
					mpi.comm_2dr, mpi.size_2dc, BFS_PARAMS::BOTTOM_UP_PRED_TAG)
			  { }

		virtual AsyncCommBuffer* alloc_buffer(bool send_or_recv) {
			return &this->pool_->get()->bottom_up_buffer;
		}
		virtual void notify_completion(bool send_or_recv, AsyncCommBuffer* buf_, int target) {
			if(send_or_recv == false) {
				BFSCommBufferImpl<TwodVertex>* buf = static_cast<BFSCommBufferImpl<TwodVertex>*>(buf_);
				buf->target_ = target;
#if VERVOSE_MODE
				g_bu_pred_comm += buf->length_ * sizeof(TwodVertex);
#endif
				this->this_->fiber_man_.submit(new BottomUpReceiver(this->this_, buf), 1);
			}
		}
	};

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

	void first_expand(int64_t root) {
		// !!root is UNSWIZZLED.!!
		const int log_size = get_msb_index(mpi.size_2d);
		const int size_mask = mpi.size_2d - 1;
#define VERTEX_OWNER(v) ((v) & size_mask)
#define VERTEX_LOCAL(v) ((v) >> log_size)

		int root_owner = (int)VERTEX_OWNER(root);
		TwodVertex root_local = VERTEX_LOCAL(root);
		TwodVertex root_src = graph_.VtoS(root);
		int root_r = root_owner % mpi.size_2dr;

		if(root_owner == mpi.rank_2d) {
			pred_[root_local] = root;
			int64_t word_idx = root_local / NBPE;
			int bit_idx = root_local % NBPE;
			new_visited_[word_idx] |= BitmapType(1) << bit_idx;
		}
		if(root_r == mpi.rank_2dr) {
			*(TwodVertex*)work_buf_ = root_src;
			cq_size_ = 1;
		}
		else {
			cq_size_ = 0;
		}
#undef VERTEX_OWNER
#undef VERTEX_LOCAL
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
		TwodVertex* recv_buf = (TwodVertex*)((cq_size_*sizeof(TwodVertex) > work_buf_size_) ?
				(work_extra_buf_ = malloc(cq_size_*sizeof(TwodVertex))) :
				work_buf_);
		MPI_Allgatherv(nq, nq_size, MpiTypeOf<TwodVertex>::type,
				recv_buf, recv_size, recv_off, MpiTypeOf<TwodVertex>::type, comm);
#if VERVOSE_MODE
		g_expand_list_comm += cq_size_ * sizeof(TwodVertex);
#endif
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
				assert ((dst - outbuf) == th_offset[tid+1]);
			} // implicit barrier
		}
		else {
			TwodVertex* new_vis_p[2]; get_visited_pointers(new_vis_p, 2, new_visited_);
			TwodVertex* old_vis_p[2]; get_visited_pointers(old_vis_p, 2, old_visited_);
			int *part_offset = s_->offset;

			for(int i = 0; i < 2; ++i) {
				part_offset[rank_z*2+1+i] = old_visited_list_size_[i] - new_visited_list_size_[i];
			}
			if(with_z) s_->sync.barrier();
			if(rank_z == 0) {
				const int num_parts = 2 * size_z;
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
#pragma omp parallel if(node_nq_size*sizeof(TwodVertex) > 16*1024*mpi.size_z)
			for(int i = 0; i < 2; ++i) {
				int max_threads = omp_get_num_threads();
				int tid = omp_get_thread_num();
				TwodVertex* dst = outbuf + part_offset[rank_z*2+i];
				TwodVertex *new_vis = new_vis_p[i], *old_vis = old_vis_p[i];
				int start, end, old_size = old_visited_list_size_[i], new_size = new_visited_list_size_[i];
				get_partition(old_size, max_threads, tid, start, end);
				int new_vis_off = std::lower_bound(
						new_vis, new_vis + new_size, old_vis[start]) - new_vis;
				int dst_off = start - new_vis_off;

				for(int c = start; c < end; ++c) {
					if(new_vis[new_vis_off] == old_vis[c]) {
						++new_vis_off;
					}
					else {
						dst[dst_off++] = (old_vis[c] & local_mask) | shifted_rc;
					}
				}
			}
		}

		return node_nq_size;
	}

	void bottom_up_expand_nq_list() {
		assert (mpi.isYdimAvailable() || (work_buf_orig_ == work_buf_));
		int node_nq_size = bottom_up_make_nq_list(
				mpi.isYdimAvailable(), TwodVertex(mpi.rank_2dr) << graph_.lgl_, (TwodVertex*)work_buf_orig_);

		if(mpi.rank_z == 0) {
			s_->offset[0] = MpiCol::allgatherv((TwodVertex*)work_buf_orig_,
					 (TwodVertex*)nq_recv_buf_, node_nq_size, mpi.comm_y, mpi.size_y);
#if VERVOSE_MODE
			g_expand_list_comm += s_->offset[0] * sizeof(TwodVertex);
#endif
		}
		if(mpi.isYdimAvailable()) s_->sync.barrier();
		int recv_nq_size = s_->offset[0];

#pragma omp parallel if(recv_nq_size > 1024*16)
		{
			const int max_threads = omp_get_num_threads();
			const int node_threads = max_threads * mpi.size_z;
			int64_t num_node_verts = (int64_t(1) << graph_.lgl_) * mpi.size_z;
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
#if VERVOSE_MODE
				g_expand_bitmap_comm += shared_bitmap_width * mpi.size_y * sizeof(BitmapType);
#endif
			}
			if(mpi.isYdimAvailable()) s_->sync.barrier();
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
				 (TwodVertex*)work_buf_, node_nq_size, mpi.comm_2dr, mpi.size_2dc);

		bitmap_or_list_ = false;
	}

	//-------------------------------------------------------------//
	// top-down search
	//-------------------------------------------------------------//

	void top_down_search() {
#if PROFILING_MODE
		profiling::TimeKeeper tk_all;
#endif
		int64_t num_edge_relax = 0;

#pragma omp parallel reduction(+:num_edge_relax)
		{
#if PROFILING_MODE
			profiling::TimeKeeper tk_all;
			profiling::TimeSpan ts_commit;
#endif
			//
			TwodVertex* cq_list = (TwodVertex*)cq_list_;
			LocalPacket* packet_array =
					thread_local_buffer_[omp_get_thread_num()]->fold_packet;
			int lgl = graph_.log_local_verts();
			uint32_t local_mask = (uint32_t(1) << lgl) - 1;
#pragma omp for
			for(TwodVertex i = 0; i < cq_size_; ++i) {
				TwodVertex src = cq_list[i];
				TwodVertex word_idx = src / NBPE;
				int bit_idx = src % NBPE;
				BitmapType row_bitmap_i = graph_.row_bitmap_[word_idx];
				BitmapType mask = BitmapType(1) << bit_idx;
				if(row_bitmap_i & mask) {
					uint32_t pred[2] = { (-(int64_t)src) >> 32, uint32_t(-(int64_t)src) };
					BitmapType low_mask = (BitmapType(1) << bit_idx) - 1;
					TwodVertex non_zero_off = graph_.row_sums_[word_idx] +
							__builtin_popcount(graph_.row_bitmap_[word_idx] & low_mask);
					SortIdx sorted_idx = graph_.sorted_idx_[non_zero_off];
					typename GraphType::BlockOffset& blk_off = graph_.blk_off[non_zero_off];
					SortIdx* blk_col_len = graph_.col_len_ + blk_off.length_start;
					TwodVertex* edge_array = graph_.edge_array_ + blk_off.edge_start;

					TwodVertex c = 0;
					for( ; sorted_idx < blk_col_len[c]; ++c, edge_array += blk_col_len[c]) {
						TwodVertex tgt = edge_array[sorted_idx];
						int dest = tgt >> lgl;
						uint32_t tgt_local = tgt & local_mask;
						LocalPacket& pk = packet_array[dest];
						if(pk.length > LocalPacket::TOP_DOWN_LENGTH-3) { // low probability
#if PROFILING_MODE
							profiling::TimeKeeper tk_commit;
#endif
							comm_.send<true>(pk.data, pk.length, dest);
#if PROFILING_MODE
							ts_commit += tk_commit;
#endif
							pk.length = 0;
						}
						if(pk.src != src) { // TODO: use conditional branch
							pk.data.t[pk.length+0] = pred[0];
							pk.data.t[pk.length+1] = pred[1];
							pk.length += 2;
						}
						pk.data.t[pk.length] = tgt_local;
					}
#if VERVOSE_MODE
					num_edge_relax += c;
#endif
				}
			} // #pragma omp for // implicit barrier

			// flush buffer
#pragma omp for nowait
			for(int target = 0; target < mpi.size_2dr; ++target) {
				for(int i = 0; i < omp_get_num_threads(); ++i) {
					LocalPacket* packet_array =
							thread_local_buffer_[i]->fold_packet;
					LocalPacket& pk = packet_array[target];
					if(pk.length > 0) {
#if PROFILING_MODE
						profiling::TimeKeeper tk_commit;
#endif
						comm_.send<true>(pk.data, pk.length, target);
#if PROFILING_MODE
						ts_commit += tk_commit;
#endif
						packet_array[target].length = 0;
						packet_array[target].src = -1;
					}
				}
#if PROFILING_MODE
				profiling::TimeKeeper tk_commit;
#endif
				comm_.send_end(target);
#if PROFILING_MODE
				ts_commit += tk_commit;
#endif
			} // #pragma omp for
#if PROFILING_MODE
			profiling::TimeSpan ts_all; ts_all += tk_all; ts_all -= ts_commit;
			extract_edge_time_ += ts_all;
			commit_time_ += ts_commit;
#endif
			// process remaining recv tasks
			fiber_man_.enter_processing();
		} // #pragma omp parallel reduction(+:num_edge_relax)
#if PROFILING_MODE
		parallel_reg_time_ += tk_all;
#endif
#if VERVOSE_MODE
		num_edge_top_down_ = num_edge_relax;
#endif
		// flush NQ buffer and count NQ total
		int max_threads = omp_get_max_threads();
		nq_size_ = nq_.stack_.size() * QueuedVertexes::SIZE;
		for(int tid = 0; tid < max_threads; ++tid) {
			ThreadLocalBuffer* tlb = thread_local_buffer_[omp_get_thread_num()];
			QueuedVertexes* buf = tlb->cur_buffer;
			if(buf != NULL) {
				nq_size_ += buf->length;
				nq_.push(buf);
			}
			tlb->cur_buffer = NULL;
		}
		int64_t send_nq_size = nq_size_;
#if PROFILING_MODE
		seq_proc_time_ += tk_all;
		MPI_Barrier(mpi.comm_2d);
		fold_competion_wait_ += tk_all;
#endif
		MPI_Allreduce(&send_nq_size, &global_nq_size_, 1, MpiTypeOf<int64_t>::type, MPI_SUM, mpi.comm_2d);
#if PROFILING_MODE
		gather_nq_time_ += tk_all;
#endif
	}

	struct TopDownReceiver : public Runnable {
		TopDownReceiver(ThisType* this__, BFSCommBufferImpl<uint32_t>* data__)
			: this_(this__), data_(data__) 	{ }
		virtual void run() {
#if PROFILING_MODE
			profiling::TimeKeeper tk_all;
#endif

			using namespace BFS_PARAMS;
			ThreadLocalBuffer* tlb = thread_local_buffer_[omp_get_thread_num()];
			QueuedVertexes* buf = tlb->cur_buffer;
			if(buf == NULL) buf = nq_empty_buffer_.get();
			BitmapType* visited = this_->new_visited_;
			int64_t* restrict const pred = this_->pred_;
			const int cur_level = this_->current_level_;
			uint32_t* stream = data_->buffer_;
			int length = data_->length;
			int64_t pred_v = -1;

			// for id converter //
			int lgr = this_->graph_.lgr_;
			int lgl = this_->graph_.lgl_;
			int lgsize = lgr + this_->graph_.lgc_;
			int64_t lmask = ((int64_t(1) << lgl) - 1);
			int r = data_->target_;
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
			this_->comm_buffer_pool_.free(static_cast<BFSCommBufferData*>(data_->obj_ptr_));
#if PROFILING_MODE
			this_->recv_proc_time_ += tk_all;
#endif
			delete this;
		}
		ThisType* const this_;
		BFSCommBufferImpl<uint32_t>* data_;
	};

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

	void swap_visited_memory() {
		std::swap(new_visited_, old_visited_);
		std::swap(new_visited_list_size_[0], old_visited_list_size_[0]);
		std::swap(new_visited_list_size_[1], old_visited_list_size_[1]);
	}

	int bottom_up_search_bitmap_process_step(
			BitmapType* phase_bitmap,
			TwodVertex phase_bmp_off,
			TwodVertex half_bitmap_width)
	{
		struct BottomUpRow {
			SortIdx orig, sorted;
		};
		int target_rank = phase_bmp_off / half_bitmap_width / 2;
		int num_bufs_nq_before = nq_.stack_.size();
		int visited_count = 0;
#if VERVOSE_MODE
		int tmp_edge_relax = 0;
#pragma omp parallel reduction(+:visited_count, tmp_edge_relax)
#else
#pragma omp parallel reduction(+:visited_count)
#endif
		{
#if PROFILING_MODE
			profiling::TimeKeeper tk_all;
			profiling::TimeSpan ts_commit;
#endif
			ThreadLocalBuffer* tlb = thread_local_buffer_[omp_get_thread_num()];
#if STREAM_UPDATE
			LocalPacket& packet = tlb->fold_packet[target_rank];
			visited_count -= packet.length;
#else
			QueuedVertexes* buf = tlb->cur_buffer;
			if(buf == NULL) buf = nq_empty_buffer_.get();
			visited_count -= buf->length;
#endif

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
#if STREAM_UPDATE
								if(packet.length == LocalPacket::BOTTOM_UP_LENGTH) {
									visited_count += packet.length;
#if PROFILING_MODE
									profiling::TimeKeeper tk_commit;
#endif
									comm_.send<false>(packet.data.b, packet.length, target_rank);
#if PROFILING_MODE
									ts_commit += tk_commit;
#endif
									packet.length = 0;
								}
								packet.data.b[packet.length+0] = src[s];
								packet.data.b[packet.length+1] = blk_vertex_base + orig;
								packet.length += 2;
#else // #if STREAM_UPDATE
								if(buf->full()) {
									nq_.push(buf); buf = nq_empty_buffer_.get();
								}
								buf->append_nocheck(src[s], blk_vertex_base + orig);
#endif // #if STREAM_UPDATE
								// end this row
#if VERVOSE_MODE
								tmp_edge_relax += c + 1;
#endif
								cur_rows[s] = rows[--num_active_rows];
							}
							else if(cur_rows[s].sorted >= next_col_len) {
								// end this row
#if VERVOSE_MODE
								tmp_edge_relax += c + 1;
#endif
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
#if STREAM_UPDATE
								if(packet.length == LocalPacket::BOTTOM_UP_LENGTH) {
									visited_count += packet.length;
#if PROFILING_MODE
									profiling::TimeKeeper tk_commit;
#endif
									comm_.send<false>(packet.data.b, packet.length, target_rank);
#if PROFILING_MODE
									ts_commit += tk_commit;
#endif
									packet.length = 0;
								}
								packet.data.b[packet.length+0] = src[s];
								packet.data.b[packet.length+1] = blk_vertex_base + orig;
								packet.length += 2;
#else // #if STREAM_UPDATE
								if(buf->full()) { // TODO: remove this branch
									nq_.push(buf); buf = nq_empty_buffer_.get();
								}
								buf->append_nocheck(src[s], blk_vertex_base + orig);
#endif // #if STREAM_UPDATE
								// end this row
#if VERVOSE_MODE
								tmp_edge_relax += c + 1;
#endif
								cur_rows[s] = rows[--num_active_rows];
							}
							else if(cur_rows[s].sorted >= next_col_len) {
								// end this row
#if VERVOSE_MODE
								tmp_edge_relax += c + 1;
#endif
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
#if STREAM_UPDATE
							if(packet.length == LocalPacket::BOTTOM_UP_LENGTH) {
								visited_count += packet.length;
#if PROFILING_MODE
								profiling::TimeKeeper tk_commit;
#endif
								comm_.send<false>(packet.data.b, packet.length, target_rank);
#if PROFILING_MODE
								ts_commit += tk_commit;
#endif
								packet.length = 0;
							}
							packet.data.b[packet.length+0] = src;
							packet.data.b[packet.length+1] = blk_vertex_base + orig;
							packet.length += 2;
#else
							if(buf->full()) {
								nq_.push(buf); buf = nq_empty_buffer_.get();
							}
							buf->append_nocheck(src, blk_vertex_base + orig);
#endif // #if STREAM_UPDATE
							// end this row
#if VERVOSE_MODE
							tmp_edge_relax += c + 1;
#endif
							rows[i] = rows[--num_active_rows];
						}
						else if(row >= next_col_len) {
							// end this row
#if VERVOSE_MODE
							tmp_edge_relax += c + 1;
#endif
							rows[i] = rows[--num_active_rows];
						}
					}
					col_edge_array += col_len[c];
				}
			} // #pragma omp for

#if STREAM_UPDATE
			visited_count += packet.length;
#else
			tlb->cur_buffer = buf;
			visited_count += buf->length;
#endif
#if PROFILING_MODE
			profiling::TimeSpan ts_all(tk_all); ts_all -= ts_commit;
			extract_edge_time_ += ts_all;
			commit_time_ += ts_commit;
#endif
		} // #pragma omp parallel reduction(+:visited_count)
#if STREAM_UPDATE
		visited_count >>= 1;
#else
		int num_bufs_nq_after = nq_.stack_.size();
		for(int i = num_bufs_nq_before; i < num_bufs_nq_after; ++i) {
			visited_count += nq_.stack_[i]->length;
		}
#endif
#if VERVOSE_MODE
		num_edge_bottom_up_ += tmp_edge_relax;
#endif
		return visited_count;
	}

	TwodVertex bottom_up_search_list_process_step(
#if VERVOSE_MODE
			int64_t& num_blocks,
#endif
			TwodVertex* phase_list,
			TwodVertex phase_size,
			int8_t* vertex_enabled,
			TwodVertex* write_list,
			TwodVertex phase_bmp_off,
			TwodVertex half_bitmap_width)
	{
		int max_threads = omp_get_max_threads();
		int target_rank = phase_bmp_off / half_bitmap_width / 2;
		int th_offset[max_threads];

#if VERVOSE_MODE
		int tmp_num_blocks = 0;
		int tmp_edge_relax = 0;
#pragma omp parallel reduction(+:tmp_num_blocks, tmp_edge_relax)
#else
#pragma omp parallel
#endif
		{
#if PROFILING_MODE
			profiling::TimeKeeper tk_all;
			profiling::TimeSpan ts_commit;
#endif
			int tid = omp_get_thread_num();
			ThreadLocalBuffer* tlb = thread_local_buffer_[tid];
#if STREAM_UPDATE
			LocalPacket& packet = tlb->fold_packet[target_rank];
#else
			QueuedVertexes* buf = tlb->cur_buffer;
			if(buf == NULL) buf = nq_empty_buffer_.get();
#endif
			int num_enabled = 0;

			struct BottomUpRow {
				SortIdx orig, sorted, orig_i;
			};

#pragma omp for schedule(static) nowait
			for(int i = 0; i < phase_size; ) {
				int blk_i_start = i;
				TwodVertex blk_idx = phase_list[i] / BFELL_SORT;
				int num_active_rows = 0;
				BottomUpRow rows[BFELL_SORT];
				SortIdx* sorted_idx = graph_.sorted_idx_;
				TwodVertex* row_sums = graph_.row_sums_;
				BitmapType* row_bitmap = graph_.row_bitmap_;
#if VERVOSE_MODE
				tmp_num_blocks++;
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
#if STREAM_UPDATE
							if(packet.length == LocalPacket::BOTTOM_UP_LENGTH) {
#if PROFILING_MODE
								profiling::TimeKeeper tk_commit;
#endif
								comm_.send<false>(packet.data.b, packet.length, target_rank);
#if PROFILING_MODE
								ts_commit += tk_commit;
#endif
								packet.length = 0;
							}
							packet.data.b[packet.length+0] = src;
							packet.data.b[packet.length+1] = blk_vertex_base + orig;
							packet.length += 2;
#else
							if(buf->full()) {
								nq_.push(buf); buf = nq_empty_buffer_.get();
							}
							buf->append_nocheck(src, blk_vertex_base + orig);
#endif
							// end this row
#if VERVOSE_MODE
							tmp_edge_relax += c + 1;
#endif
							rows[i] = rows[--num_active_rows];
						}
						else if(row >= next_col_len) {
							// end this row
#if VERVOSE_MODE
							tmp_edge_relax += c + 1;
#endif
							rows[i] = rows[--num_active_rows];
							++num_enabled;
						}
					}
					col_edge_array += col_len[c];
				}
			} // #pragma omp for schedule(static) nowait
			th_offset[tid+1] = num_enabled;

#pragma omp barrier
#pragma omp single
			{
				th_offset[0] = 0;
				for(int i = 0; i < max_threads; ++i) {
					th_offset[i+1] += th_offset[i];
				}
				assert (th_offset[max_threads] <= phase_size);
			} // implicit barrier

			// make new list to send
			int offset = th_offset[tid];

#pragma omp for schedule(static) nowait
			for(int i = 0; i < phase_size; ++i) {
				if(vertex_enabled[i]) {
					write_list[offset++] = phase_list[i];
				}
			}

#if !STREAM_UPDATE
			tlb->cur_buffer = buf;
#endif
#if PROFILING_MODE
			profiling::TimeSpan ts_all(tk_all); ts_all -= ts_commit;
			extract_edge_time_ += ts_all;
			commit_time_ += ts_commit;
#endif
		} // #pragma omp parallel reduction(+:tmp_num_blocks)
#if VERVOSE_MODE
		num_blocks += tmp_num_blocks;
		num_edge_bottom_up_ += tmp_edge_relax;
#endif
		return th_offset[max_threads];
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

	void bottom_up_finalize() {
#if PROFILING_MODE
		profiling::TimeKeeper tk_all;
#endif

#pragma omp parallel
		{
#pragma omp for
			for(int target = 0; target < mpi.size_2dc; ++target) {
#if PROFILING_MODE
				profiling::TimeKeeper tk_all;
				profiling::TimeSpan ts_commit;
#endif
				for(int i = 0; i < omp_get_num_threads(); ++i) {
					LocalPacket* packet_array =
							thread_local_buffer_[i]->fold_packet;
					LocalPacket& pk = packet_array[target];
					if(pk.length > 0) {
#if PROFILING_MODE
						profiling::TimeKeeper tk_commit;
#endif
						comm_.send<true>(pk.data.b, pk.length, target);
#if PROFILING_MODE
						ts_commit += tk_commit;
#endif
						packet_array[target].length = 0;
					}
				}
#if PROFILING_MODE
				profiling::TimeKeeper tk_commit;
#endif
				comm_.send_end(target);
#if PROFILING_MODE
				ts_commit += tk_commit;
				profiling::TimeSpan ts_all(tk_all); ts_all -= ts_commit;
				extract_edge_time_ += ts_all;
				commit_time_ += ts_commit;
#endif
			}// #pragma omp for

			// update pred
			fiber_man_.enter_processing();
		} // #pragma omp parallel
#if PROFILING_MODE
		parallel_reg_time_ += tk_all;
#endif
	}

#if BF_DEEPER_ASYNC
	class BottomUpComm :  public AsyncCommHandler {
	public:
		BottomUpComm(ThisType* this__, MPI_Comm mpi_comm__) {
			this_ = this__;
			mpi_comm = mpi_comm__;
			int size_cmask = mpi.size_2dc - 1;
			send_to = (mpi.rank_2dc - 1) & size_cmask;
			recv_from = (mpi.rank_2dc + 1) & size_cmask;
			total_phase = mpi.size_2dc*2;
			for(int i = 0; i < DNBUF; ++i) {
				req[i] = MPI_REQUEST_NULL;
			}
		}
		void begin() {
			recv_phase = 0;
			proc_phase = 0;
			finished = 0;
			processed_phase = 0;
			complete_count = 0;
			cur_phase = -1;
			this_->comm_.register_handler(this);
		}
		void advance() {
			int phase = proc_phase++;
			while(recv_phase < phase) sched_yield();
		}
		void finish() {
			while(!finished) sched_yield();
		}
		virtual void probe() {
			if(proc_phase <= cur_phase) {
				// wait for processing
				return ;
			}
			if(cur_phase == -1) {
				set_buffer(-1); ++cur_phase;
			}
			if(processed_phase < proc_phase) {
				set_buffer(processed_phase++);
			}

			assert (processed_phase >= cur_phase);

			MPI_Request* req_ptr = req + (cur_phase & BUFMASK) * 2;
			int index, flag;
			MPI_Status status;
			MPI_Testany(2, req_ptr, &index, &flag, &status);

			// check req_ptr has at least one active handle.
			assert (flag == false || index != MPI_UNDEFINED);

			if(flag == 0 || index == MPI_UNDEFINED) {
				continue;
			}

			assert (index == 0 || index == 1);

			++complete_count;
			if(index) { // recv
				// tell processor completion
				recv_complete(cur_phase, &status);
				recv_phase = cur_phase + 1;
			}
			if(complete_count == 2) {
				// next phase
				complete_count = 0;
				if(++cur_phase == total_phase) {
					finished = true;
					this_->comm_.remove_handler(this);
#if VERVOSE_MODE
					fprintf(IMD_OUT, "bottom-up communication thread finished.\n");
#endif
				}
			}
		}

	protected:
		enum { NBUF = BFS_PARAMS::BOTTOM_UP_BUFFER, DNBUF = NBUF*2, BUFMASK = NBUF-1 };
		ThisType* this_;
		MPI_Comm mpi_comm;
		int send_to, recv_from;
		int total_phase;
		MPI_Request req[DNBUF];
		int complete_count;
		int processed_phase;
		int cur_phase;
		volatile int recv_phase;
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
#if VERVOSE_MODE
				g_bu_bitmap_comm += half_bitmap_width * sizeof(BitmapType);
#endif
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
				MPI_Isend(send, send_size, MpiTypeOf<TwodVertex>::type, send_to, 0, mpi_comm, req_ptr);
#if VERVOSE_MODE
				g_bu_list_comm += send_size * sizeof(BitmapType);
#endif
			}
			if(recv_phase <= total_phase + 1) {
				TwodVertex* recv = list_buffer[recv_phase & BUFMASK];
				MPI_Request* req_ptr = req + ((phase + 1) & BUFMASK) * 2 + 1;
				if(recv_phase >= total_phase) {
					recv = new_vis[recv_phase - total_phase];
				}
				MPI_Irecv(recv, buffer_size, MpiTypeOf<TwodVertex>::type, recv_from, 0, mpi_comm, req_ptr);
			}
		}
	};

	void bottom_up_search_bitmap() {
		using namespace BFS_PARAMS;
		enum { NBUF = BOTTOM_UP_BUFFER, BUFMASK = NBUF-1 };
		int half_bitmap_width = get_bitmap_size_local() / 2;
		assert (work_buf_size_ >= half_bitmap_width * NBUF);
		BitmapType* bitmap_buffer[NBUF]; get_visited_pointers(bitmap_buffer, NBUF, work_buf_);
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
#if PROFILING_MODE
			profiling::TimeKeeper tk_all;
#endif
			comm.advance();
#if PROFILING_MODE
			comm_wait_time_ += tk_all;
#endif
		}
		// wait for local_visited is received.
#if PROFILING_MODE
		profiling::TimeKeeper tk_all;
#endif
		comm.advance();
		comm.finish();
#if PROFILING_MODE
		comm_wait_time_ += tk_all;
#endif
		bottom_up_finalize();
		bottom_up_gather_nq_size(visited_count);
#if VERVOSE_MODE
		botto_up_print_stt(0, 0, visited_count);
#endif
	}

	int bottom_up_search_list() {
		using namespace BFS_PARAMS;
		enum { NBUF = BOTTOM_UP_BUFFER, BUFMASK = NBUF-1 };
		int half_bitmap_width = get_bitmap_size_local() / 2;
		int buffer_size = half_bitmap_width * sizeof(BitmapType) / sizeof(TwodVertex);
		TwodVertex* list_buffer[NBUF]; get_visited_pointers(list_buffer, NBUF, work_buf_);
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
					num_blocks,
#endif
					phase_list, phase_size, vertex_enabled, write_list, phase_bmp_off, half_bitmap_width);
#if VERVOSE_MODE
			num_vertexes += phase_size;
#endif
			list_size[write_phase & BUFMASK] = write_size;
			visited_count[(phase/2 + comm_rank) & comm_size_mask] += write_size;
#if PROFILING_MODE
			profiling::TimeKeeper tk_all;
#endif
			comm.advance();
#if PROFILING_MODE
			comm_wait_time_ += tk_all;
#endif
		}
		// wait for local_visited is received.
#if PROFILING_MODE
		profiling::TimeKeeper tk_all;
#endif
		comm.advance();
		comm.finish();
#if PROFILING_MODE
		comm_wait_time_ += tk_all;
#endif
		bottom_up_finalize();
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
			bitmap_buffer[i] = (BitmapType*)work_buf_ + half_bitmap_width*i;
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

	struct BottomUpReceiver : public Runnable {
		BottomUpReceiver(ThisType* this__, BFSCommBufferImpl<TwodVertex>* data__)
			: this_(this__), data_(data__) 	{ }
		virtual void run() {
#if PROFILING_MODE
			profiling::TimeKeeper tk_all;
#endif
			int lgl = this_->graph_.lgl_;
			int lgsize = this_->graph_.lgr_ + this_->graph_.lgc_;
			TwodVertex lmask = (TwodVertex(1) << lgl) - 1;
			int64_t cshifted = int64_t(data_->target_) << graph_.lgr_;
			int64_t levelshifted = int64_t(this_->current_level_) << 48;
			int64_t const_mask = cshifted | levelshifted;
			TwodVertex* buffer = data_->buffer_;
			int length = data_->length_ / 2;
			for(int i = 0; i < length; ++i) {
				TwodVertex pred_dst = buffer[i*2+0];
				TwodVertex tgt_local = buffer[i*2+1] & lmask;
				int64_t pred_v = ((pred_dst & lmask) << lgsize) | const_mask | (pred_dst >> lgl);
				assert (pred_[tgt_local] == -1);
				pred_[tgt_local] = pred_v;
			}

			this_->comm_buffer_pool_.free(static_cast<BFSCommBufferData*>(data_->obj_ptr_));
#if PROFILING_MODE
			this_->recv_proc_time_ += tk_all;
#endif
			delete this;
		}
		ThisType* const this_;
		BFSCommBufferImpl<uint32_t>* data_;
	};

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
#if VERVOSE_MODE
	void printTime(const char* fmt, double* sum, double* max, int idx) {
		fprintf(IMD_OUT, fmt, sum[idx] / mpi.size_2d * 1000.0,
				diff_percent(max[idx], sum[idx], mpi.size_2d));
	}
	void printCounter(const char* fmt, int64_t* sum, int64_t* max, int idx) {
		fprintf(IMD_OUT, fmt, to_mega(sum[idx] / mpi.size_2d),
				diff_percent(max[idx], sum[idx], mpi.size_2d));
	}
#endif
	void prepare_sssp() { }
	void run_sssp(int64_t root, int64_t* pred) { }
	void end_sssp() { }

	// members

	FiberManager fiber_man_;
	AsyncCommManager comm_;
	TopDownCommHandler top_down_comm_;
#if STREAM_UPDATE
	BottomUpCommHandler bottom_up_comm_;
#endif
	ThreadLocalBuffer** thread_local_buffer_;
	CommBufferPool comm_buffer_pool_;
	memory::ConcurrentPool<QueuedVertexes> nq_empty_buffer_;
	memory::ConcurrentStack<QueuedVertexes*> nq_;

	// switch parameters
	double demon_to_bottom_up; // alpha
	double demon_to_top_down; // beta

	// cq_list_ is a pointer to work_buf_ or work_extra_buf_
	TwodVertex* cq_list_;
	TwodVertex cq_size_;
	int nq_size_;
	int max_nq_size_; // available only in the bottom-up phase
	int64_t global_nq_size_;
	int64_t prev_global_nq_size_;

	// size = local bitmap width
	// These two buffer is swapped at the beginning of every backward step
	void* new_visited_; // shared memory but point to the local portion
	void* old_visited_; // shared memory but point to the local portion
	// size = 2
	int new_visited_list_size_[2];
	int old_visited_list_size_[2];
	bool bitmap_or_list_;

	// size = local bitmap width / 2 * NBUF
	// work_buf_orig_ + half_bitmap_width_ * NBUF * mpi.rank_z
	void* work_buf_; // shared memory but point to the local portion
	void* work_buf_orig_; // shared memory
	void* work_extra_buf_;
	int64_t work_buf_size_; // in bytes
	int64_t work_extra_buf_size_;

	BitmapType* shared_visited_; // shared memory
	TwodVertex* nq_recv_buf_; // shared memory

	int64_t* pred_; // passed from main method

	struct SharedDataSet {
		memory::SpinBarrier sync;
		int *offset; // max(max_threads*2+1, 2*mpi.size_z+1)

		SharedDataSet()
			: sync(mpi.size_z)
			//, th_sync(mpi.size_z * omp_get_max_threads())
		{ }
	} *s_; // shared memory

	struct DynamicDataSet {
#if AVOID_BUSY_WAIT
		pthread_mutex_t avoid_busy_wait_sync_;
#endif
	} *d_;

	int log_local_bitmap_;

	int current_level_;
	bool forward_or_backward_;

#if VERVOSE_MODE
	int64_t num_edge_top_down_;
	int64_t num_edge_bottom_up_;
#endif
	struct {
		void* thread_local_;
		void* shared_memory_;
	} buffer_;
#if PROFILING_MODE
	profiling::TimeSpan extract_edge_time_;
	profiling::TimeSpan parallel_reg_time_;
	profiling::TimeSpan commit_time_;
	profiling::TimeSpan comm_wait_time_;
	profiling::TimeSpan fold_competion_wait_;
	profiling::TimeSpan recv_proc_time_;
	profiling::TimeSpan gather_nq_time_;
	profiling::TimeSpan seq_proc_time_;
#endif
#if BIT_SCAN_TABLE
	uint8_t bit_scan_table_[BFS_PARAMS::BIT_SCAN_TABLE_SIZE];
#endif
};



template <typename TwodVertex, typename PARAMS>
void BfsBase<TwodVertex, PARAMS>::
	run_bfs(int64_t root, int64_t* pred)
{
	using namespace BFS_PARAMS;
#if SWITCH_FUJI_PROF
	fapp_start("initialize", 0, 0);
	start_collection("initialize");
#endif
	pred_ = pred;
#if VERVOSE_MODE
	double tmp = MPI_Wtime();
	double start_time = tmp;
	double prev_time = tmp;
	double expand_time = 0.0, fold_time = 0.0;
	g_tp_comm = g_bu_pred_comm = g_bu_bitmap_comm = g_bu_list_comm = g_expand_bitmap_comm = g_expand_list_comm = 0;
	int64_t total_edge_top_down = 0;
	int64_t total_edge_bottom_up = 0;
#endif

	initialize_memory(pred);

#if VERVOSE_MODE
	if(mpi.isMaster()) fprintf(IMD_OUT, "Time of initialize memory: %f ms\n", (MPI_Wtime() - prev_time) * 1000.0);
	prev_time = MPI_Wtime();
#endif

	bitmap_or_list_ = false;
	current_level_ = 0;
	forward_or_backward_ = true; // begin with forward search
	prev_global_nq_size_ = 1;
	int64_t global_visited_vertices = 0;

	first_expand(root);

#if VERVOSE_MODE
	tmp = MPI_Wtime();
	double cur_expand_time = tmp - prev_time;
	expand_time += cur_expand_time; prev_time = tmp;
#endif

#if SWITCH_FUJI_PROF
	stop_collection("initialize");
	fapp_stop("initialize", 0, 0);
	char *prof_mes[] = { "bottom-up", "top-down" };
#endif

	while(true) {
		++current_level_;
#if VERVOSE_MODE
		num_edge_top_down_ = 0;
		num_edge_bottom_up_ = 0;
#if PROFILING_MODE
		fiber_man_.reset_wait_time();
#endif // #if PROFILING_MODE
#endif // #if VERVOSE_MODE
#if SWITCH_FUJI_PROF
		fapp_start(prof_mes[(int)forward_or_backward_], 0, 0);
		start_collection(prof_mes[(int)forward_or_backward_]);
#endif
		// search phase //
		fiber_man_.begin_processing();
		if(forward_or_backward_) { // forward
			comm_.begin_comm(TOP_DOWN_COMM_H);
			assert (bitmap_or_list_ == false);
			top_down_search();
			// release extra buffer
			if(work_extra_buf_ != NULL) { free(work_extra_buf_); work_extra_buf_ = NULL; }
		}
		else { // backward
			swap_visited_memory();
			comm_.begin_comm(BOTTOM_UP_COMM_H);
			if(bitmap_or_list_) { // bitmap
				bottom_up_search_bitmap();
			}
			else { // list
				bottom_up_search_list();
			}
		}

		global_visited_vertices += global_nq_size_;
		assert(comm_.check_num_send_buffer());

#if VERVOSE_MODE
		tmp = MPI_Wtime();
		double cur_fold_time = tmp - prev_time;
		fold_time += cur_fold_time; prev_time = tmp;
		total_edge_top_down += num_edge_top_down_;
		total_edge_bottom_up += num_edge_bottom_up_;
#if PROFILING_MODE
		if(forward_or_backward_)
			extract_edge_time_.submit("forward edge", current_level_);
		else
			extract_edge_time_.submit("backward edge", current_level_);

		parallel_reg_time_.submit("parallel region", current_level_);
		commit_time_.submit("extract commit", current_level_);
		if(!forward_or_backward_) { // backward
			comm_wait_time_.submit("bottom-up communication wait", current_level_);
			fold_competion_wait_.submit("fold completion wait", current_level_);
		}
		recv_proc_time_.submit("recv proc", current_level_);
		gather_nq_time_.submit("gather NQ info", current_level_);
		seq_proc_time_.submit("sequential processing", current_level_);
		comm_.submit_prof_info("send comm wait", current_level_);
		fiber_man_.submit_wait_time("fiber man wait", current_level_);

		if(forward_or_backward_)
			profiling::g_pis.submitCounter(num_edge_top_down_, "top-down edge relax", current_level_);
		else
			profiling::g_pis.submitCounter(num_edge_bottom_up_, "bottom-up edge relax", current_level_);
#endif // #if PROFILING_MODE
#endif // #if VERVOSE_MODE
#if SWITCH_FUJI_PROF
		stop_collection(prof_mes[(int)forward_or_backward_]);
		fapp_stop(prof_mes[(int)forward_or_backward_], 0, 0);
#endif
		if(global_nq_size_ == 0)
			break;
#if SWITCH_FUJI_PROF
		fapp_start("expand", 0, 0);
		start_collection("expand");
#endif
		int64_t global_unvisited_vertices = graph_.num_global_vertices_ - global_visited_vertices;
		bool next_forward_or_backward = forward_or_backward_;
		if(global_nq_size_ > prev_global_nq_size_) { // growing
			if(forward_or_backward_ // forward ?
				&& global_nq_size_ > graph_.num_global_verts_ / demon_to_bottom_up // NQ is large ?
				) { // switch to backward
				next_forward_or_backward = false;
			}
		}
		else { // shrinking
			if(forward_or_backward_ == false // backward ?
				&& (double)global_nq_size_ < global_unvisited_vertices / demon_to_top_down // NQ is small ?
				) { // switch to forward
				next_forward_or_backward = true;
			}
		}
#if VERVOSE_MODE
		int send_num_bufs[2] = { comm_buffer_pool_.size(), nq_empty_buffer_.size() };
		int sum_num_bufs[2], max_num_bufs[2];
		MPI_Reduce(send_num_bufs, sum_num_bufs, 2, MPI_INT, MPI_SUM, 0, mpi.comm_2d);
		MPI_Reduce(send_num_bufs, max_num_bufs, 2, MPI_INT, MPI_MAX, 0, mpi.comm_2d);
		if(mpi.isMaster()) {
			double nq_rate = (double)global_nq_size_ / graph_.num_global_verts_;
			double unvis_rate = (double)global_unvisited_vertices / graph_.num_global_vertices_;
			double unvis_nq_rate = (double)global_unvisited_vertices / global_nq_size_;
			double time_of_level = cur_expand_time + cur_fold_time;
			fprintf(IMD_OUT, "=== Level %d complete ===\n", current_level_);
			fprintf(IMD_OUT, "Direction %s\n", forward_or_backward_ ? "top-down" : "bottom-up");

			fprintf(IMD_OUT, "Expand Time: %f ms\n", cur_expand_time * 1000.0);
			fprintf(IMD_OUT, "Fold Time: %f ms\n", cur_fold_time * 1000.0);
			fprintf(IMD_OUT, "Level Total Time: %f ms\n", time_of_level * 1000.0);

			fprintf(IMD_OUT, "NQ %"PRId64", 1/ %f, %f %% of global\n"
							"Unvisited %"PRId64", 1/ %f, %f of global, 1/ %f, %f of NQ\n",
						global_nq_size_, 1/nq_rate, nq_rate*100,
						global_unvisited_vertices, 1/nq_rate, nq_rate*100, 1/nq_rate, nq_rate*100);

			int64_t edge_relaxed = forward_or_backward_ ? num_edge_top_down_ : num_edge_bottom_up_;
			fprintf(IMD_OUT, "Edge relax: %"PRId64", %f %%, %f M/s (Level), %f M/s (Fold)\n",
					edge_relaxed, (double)edge_relaxed / graph_.num_global_edges_ * 100.0,
					to_mega(edge_relaxed) / time_of_level,
					to_mega(edge_relaxed) / cur_fold_time);

			int64_t total_cb_size = int64_t(sum_num_bufs[0]) * COMM_BUFFER_SIZE;
			int64_t max_cb_size = int64_t(max_num_bufs[0]) * COMM_BUFFER_SIZE;
			int64_t total_qb_size = int64_t(sum_num_bufs[1]) * BUCKET_UNIT_SIZE;
			int64_t max_qb_size = int64_t(max_num_bufs[1]) * BUCKET_UNIT_SIZE;
			fprintf(IMD_OUT, "Comm buffer: %f MB per node, Max %f %%+\n",
					to_mega(total_cb_size / mpi.size_2d), diff_percent(max_cb_size, total_cb_size, mpi.size_2d));
			fprintf(IMD_OUT, "Queue buffer: %f MB per node, Max %f %%+\n",
					to_mega(total_qb_size / mpi.size_2d), diff_percent(max_qb_size, total_qb_size, mpi.size_2d));

			if(next_forward_or_backward != forward_or_backward_) {
				if(forward_or_backward_)
					fprintf(IMD_OUT, "Change Direction: top-down -> bottom-up\n");
				else
					fprintf(IMD_OUT, "Change Direction: bottom-up -> top-down\n");
			}

			fprintf(IMD_OUT, "=== === === === ===\n");
		}
#endif
		if(next_forward_or_backward == forward_or_backward_) {
			if(forward_or_backward_)
				top_down_expand();
			else
				bottom_up_expand();
		} else {
			if(forward_or_backward_)
				top_down_switch_expand();
			else
				bottom_up_switch_expand();
		}
		forward_or_backward_ = next_forward_or_backward;
		prev_global_nq_size_ = global_nq_size_;
#if SWITCH_FUJI_PROF
		stop_collection("expand");
		fapp_stop("expand", 0, 0);
#endif
#if VERVOSE_MODE
		tmp = MPI_Wtime();
		cur_expand_time = tmp - prev_time;
		expand_time += cur_expand_time; prev_time = tmp;
#endif
	} // while(true) {
#if VERVOSE_MODE
	if(mpi.isMaster()) fprintf(IMD_OUT, "Time of BFS: %f ms\n", (MPI_Wtime() - start_time) * 1000.0);
	int64_t total_edge_relax = total_edge_top_down + total_edge_bottom_up;
	int time_cnt = 2, cnt_cnt = 9;
	double send_time[time_cnt] = { fold_time, expand_time }, sum_time[time_cnt], max_time[time_cnt];
	int64_t send_cnt[cnt_cnt] = { g_tp_comm, g_bu_pred_comm, g_bu_bitmap_comm,
			g_bu_list_comm, g_expand_bitmap_comm, g_expand_list_comm,
			total_edge_top_down, total_edge_bottom_up, total_edge_relax };
	int64_t sum_cnt[cnt_cnt], max_cnt[cnt_cnt];
	MPI_Reduce(send_time, sum_time, time_cnt, MPI_DOUBLE, MPI_SUM, 0, mpi.comm_2d);
	MPI_Reduce(send_time, max_time, time_cnt, MPI_DOUBLE, MPI_MAX, 0, mpi.comm_2d);
	MPI_Reduce(send_cnt, sum_cnt, cnt_cnt, MpiTypeOf<int64_t>::type, MPI_SUM, 0, mpi.comm_2d);
	MPI_Reduce(send_cnt, max_cnt, cnt_cnt, MpiTypeOf<int64_t>::type, MPI_MAX, 0, mpi.comm_2d);
	if(mpi.isMaster()) {
		printTime("Avg time of fold: %f ms, %f %%+\n", sum_time, max_time, 0);
		printTime("Avg time of expand: %f ms, %f %%+\n", sum_time, max_time, 1);
		printCounter("Avg top-down fold recv: %f MiB, %f %%+\n", sum_cnt, max_cnt, 0);
		printCounter("Avg bottom-up pred update recv: %f MiB, %f %%+\n", sum_cnt, max_cnt, 1);
		printCounter("Avg bottom-up bitmap send: %f MiB, %f %%+\n", sum_cnt, max_cnt, 2);
		printCounter("Avg bottom-up list send: %f MiB, %f %%+\n", sum_cnt, max_cnt, 3);
		printCounter("Avg expand bitmap recv: %f MiB, %f %%+\n", sum_cnt, max_cnt, 4);
		printCounter("Avg expand list recv: %f MiB, %f %%+\n", sum_cnt, max_cnt, 5);
		printCounter("Avg top-down traversed edges: %f MiB, %f %%+\n", sum_cnt, max_cnt, 6);
		printCounter("Avg bottom-up traversed edges: %f MiB, %f %%+\n", sum_cnt, max_cnt, 7);
		printCounter("Avg total relaxed traversed: %f MiB, %f %%+\n", sum_cnt, max_cnt, 8);
	}
#endif
}

#endif /* BFS_HPP_ */
