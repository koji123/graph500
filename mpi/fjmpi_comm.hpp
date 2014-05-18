/*
 * fjmpi_comm.hpp
 *
 *  Created on: 2014/05/18
 *      Author: ueno
 */

#ifndef FJMPI_COMM_HPP_
#define FJMPI_COMM_HPP_
#ifdef ENABLE_FJMPI_RDMA

#include <mpi-ext.h>

#include "abstract_comm.hpp"

class FJMpiAlltoallCommunicatorBase : public AlltoallCommunicator {
public:
	enum {
		MAX_SYSTEM_MEMORY = 16*1024*1024,
		MAX_RDMA_BUFFER = 256,
		RDMA_BUF_SIZE = MAX_SYSTEM_MEMORY / MAX_RDMA_BUFFER,
		MAX_FLYING_REQ = 4, // maximum is 14 due to the limitation of FJMPI
		INITIAL_RADDR_TABLE_SIZE = 4,
		SYSTEM_TAG = 0,
		FIRST_DATA_TAG = 1,
	};
private:
	enum STATE_FLAG {
		INVALIDATED = 0,
		READY = 1,
		COMPLETE = 2,
	};

	class RankMap {
		typedef std::pair<int,int> ElementType;
		struct Compare {
			bool operator()(const ElementType& l, int r) {
				return l.first < r;
			}
		};
	public:
		typedef std::vector<ElementType>::iterator iterator;

		iterator lower_bound(int pid) {
			return std::lower_bound(vec.begin(), vec.end(), pid, Compare());
		}

		bool match(iterator it, int pid) {
			if(it == vec.end() || it->first != pid) return false;
			return true;
		}

		void add(iterator it, int pid, int rank) {
			vec.insert(it, ElementType(pid, rank));
		}

	private:
		std::vector<ElementType> vec;
	};

	struct BufferState {
		uint16_t state; // current state of the buffer
		uint16_t memory_id; // memory id of the RDMA buffer
		union {
			uint64_t offset; // offset to the buffer starting address
			uint64_t length; // length of the received data in bytes
		};
	};

	struct CommTarget {
		int proc_index;
		int put_flag;
		uint64_t remote_buffer_state;

		std::deque<CommunicationBuffer*> send_queue;

		CommunicationBuffer* send_buf[MAX_FLYING_REQ];
		CommunicationBuffer* recv_buf[MAX_FLYING_REQ];

		// take care the overflow of these counters
		unsigned int send_count; // the next buffer index is calculated from this value
		unsigned int recv_count; //   "
		unsigned int recv_complete_count;

		CommTarget() {
			proc_index = put_flag = 0;
			remote_buffer_state = 0;
			for(int i = 0; i < MAX_FLYING_REQ; ++i) {
				send_buf[i] = NULL;
				recv_buf[i] = NULL;
			}
			send_count = recv_count = recv_complete_count = 0;
		}
	};

	struct InternalCommunicator {
		MPI_Comm base_communicator;
		AlltoallBufferHandler* handler;
		int size, rank;
		int num_nics_to_use;
		//! pid -> rank
		RankMap rank_map;
		//! index = rank in the base communicator
		std::vector<CommTarget> proc_info;
		BufferState* local_buffer_state;
		int num_recv_active;
		int num_send_active;
		int num_pending_send;

		BufferState& send_buffer_state(int rank, int idx) {
			return local_buffer_state[offset_of_send_buffer_state(rank, idx)];
		}

		BufferState& get_recv_buffer_state(int rank, int idx) {
			return local_buffer_state[get_recv_buffer_state_offset(rank, idx)];
		}
	};

public:
	FJMpiAlltoallCommunicatorBase() {
		c_ = NULL;
		fix_system_memory_ = false;
		remote_address_table_ = NULL;
		num_procs_ = 0;
		num_address_per_proc_ = 0;
		for(int i = 0; i < MAX_RDMA_BUFFER; ++i) {
			rdma_buffer_pointers_[i] = NULL;
			free_memory_ids_[i] = MAX_RDMA_BUFFER;
			local_address_table_[i] = 0;
		}
		MPI_Comm_group(MPI_COMM_WORLD, &world_group_);
		system_rdma_mem_size_ = 0;
	}
	virtual ~FJMpiAlltoallCommunicatorBase() {
		free(remote_address_table_); remote_address_table_ = NULL;
		for(int i = 0; i < MAX_RDMA_BUFFER; ++i) {
			if(rdma_buffer_pointers_[i] != NULL) {
				FJMPI_Rdma_dereg_mem(i);
				free(rdma_buffer_pointers_[i]); rdma_buffer_pointers_[i] = NULL;
			}
		}
		MPI_Group_free(&world_group_);
	}
	virtual void send(CommunicationBuffer* data, int target) {
		c_->proc_info[target].send_queue.push_back(data);
		c_->num_pending_send++;
		set_send_buffer(target);
	}
	virtual AlltoallSubCommunicator reg_comm(AlltoallCommParameter parm) {
		if(fix_system_memory_) {
			throw_exception("reg_comm is called after data memory allocation");
		}
		int idx = internal_communicators_.size();
		internal_communicators_.push_back(InternalCommunicator());
		InternalCommunicator& c = internal_communicators_.back();

		// initialize c
		c.base_communicator = parm.base_communicator;
		c.handler = parm.handler;
		MPI_Comm_rank(c.base_communicator, &c.rank);
		MPI_Comm_size(c.base_communicator, &c.size);
		c.num_nics_to_use = parm.num_nics_to_use;
		c.proc_info.resize(c.size, CommTarget());

		if(c.num_nics_to_use > 4) {
			c.num_nics_to_use = 4;
		}
		int remote_nic = c.rank % c.num_nics_to_use;
		int local_nic[4] = {
			FJMPI_RDMA_LOCAL_NIC0,
			FJMPI_RDMA_LOCAL_NIC1,
			FJMPI_RDMA_LOCAL_NIC2,
			FJMPI_RDMA_LOCAL_NIC3
		};
		MPI_Group comm_group;
		MPI_Comm_group(c.base_communicator, &comm_group);
		int* ranks1 = new int[c.size];
		int* ranks2 = new int[c.size];
		for(int i = 0; i < c.size; ++i) {
			ranks1[i] = i;
		}
		MPI_Group_translate_ranks(comm_group, c.size, ranks1, world_group_, ranks2);

		for(int i = 0; i < c.size; ++i) {
			int pid = ranks2[i];
			int proc_idx;
			RankMap::iterator it = proc_index_map_.lower_bound(pid);
			if(proc_index_map_.match(it, pid)) {
				proc_idx = it->second;
			}
			else {
				proc_idx = proc_info_.size();
				proc_index_map_.add(it, pid, proc_idx);
				proc_info_.push_back(pid);
			}

			c.rank_map.add(c.rank_map.lower_bound(pid), pid, i);
			c.proc_info[i].proc_index = proc_idx;
			c.proc_info[i].put_flag = local_nic[i % c.num_nics_to_use] |
					remote_nic | FJMPI_RDMA_IMMEDIATE_RETURN | FJMPI_RDMA_PATH0;
		}

		delete [] ranks1; ranks1 = NULL;
		delete [] ranks2; ranks2 = NULL;
		MPI_Group_free(&comm_group);

		c.local_buffer_state = NULL;
		c.num_recv_active = c.num_send_active = c.num_pending_send = 0;

		// allocate buffer state memory
		if(rdma_buffer_pointers_[0] == NULL) {
			initialize_rdma_buffer();
		}
		int offset = system_rdma_mem_size_;
		c.local_buffer_state = get_pointer<BufferState>(rdma_buffer_pointers_[0], offset);
		system_rdma_mem_size_ += sizeof(BufferState) * buffer_state_offset(c.size);

		if(system_rdma_mem_size_ > RDMA_BUF_SIZE) {
			throw_exception("no system RDMA memory");
		}

		// initialize buffer state
		for(int p = 0; p < buffer_state_offset(c.size); ++p) {
			c.local_buffer_state[p].state = INVALIDATED;
		}

		// get the remote buffer state address
		int* state_offset = new int[c.size];
		MPI_Allgather(&offset, 1, MpiTypeOf<int>::type,
				state_offset, 1, MpiTypeOf<int>::type, c.base_communicator);

		for(int i = 0; i < c.size; ++i) {
			int proc_index = c.proc_info[i].proc_index;
			c.proc_info[i].remote_buffer_state = get_remote_address(proc_index, 0, state_offset[i]);
		}

		delete [] state_offset; state_offset = NULL;

		return idx;
	}
	virtual AlltoallBufferHandler* begin(AlltoallSubCommunicator sub_comm) {
		c_ = &internal_communicators_[sub_comm];
		c_->num_recv_active = c_->num_send_active = c_->size;
		return c_->handler;
	}
	//! @return finished
	virtual bool probe() {

		if(c_->num_recv_active == 0 && c_->num_send_active == 0) {
			// finished
			c_->handler->finished();
			return true;
		}

		// process receive completion
		for(int p = 0; p < c_->size; ++p) {
			check_recv_completion(p);
			set_recv_buffer(p);
		}

		// process send completion
		int nics[4] = {
			FJMPI_RDMA_NIC0,
			FJMPI_RDMA_NIC1,
			FJMPI_RDMA_NIC2,
			FJMPI_RDMA_NIC3
		};
		for(int nic = 0; nic < c_->num_nics_to_use; ++nic) {
			while(true) {
				struct FJMPI_Rdma_cq cq;
				int ret = FJMPI_Rdma_poll_cq(nics[nic], &cq);
				if(ret == FJMPI_RDMA_NOTICE) {
					if(cq.tag == SYSTEM_TAG) {
						// nothing to do
					}
					else if(cq.tag >= FIRST_DATA_TAG && cq.tag < FIRST_DATA_TAG + MAX_FLYING_REQ){
						RankMap::iterator it = c_->rank_map.lower_bound(cq.pid);
						assert(c_->rank_map.match(it, cq.pid));
						int rank = it->second;
						int buf_idx = cq.tag - FIRST_DATA_TAG;
						CommunicationBuffer*& comm_buf = c_->proc_info[rank].send_buf[buf_idx];
						bool completion_message = (comm_buf->length_ == 0);
						c_->handler->free_buffer(comm_buf);
						comm_buf = NULL;
						if(completion_message) {
							c_->num_send_active--;
						}
						else {
							set_send_buffer(rank);
						}
					}
					else {
						// impossible
					}
				}
				else if(ret == FJMPI_RDMA_HALFWAY_NOTICE) {
					// impossible because we do not use notify flag at all
				}
				else if(ret == 0) {
					// no completion
					break;
				}
			}
		}

		return false;
	}
	virtual int get_comm_size() {
		return c_->size;
	}
#ifndef NDEBUG
	bool check_num_send_buffer() { return (c_->num_pending_send == 0); }
#endif
#if PROFILING_MODE
	void submit_prof_info(int number) {
		int num_rdma_buffers = MAX_RDMA_BUFFER - num_free_memory_;
		profiling::g_pis.submitCounter(num_rdma_buffers, "num_rdma_buffers", number);
	}
#endif

	virtual int memory_id_of(CommunicationBuffer* comm_buf) = 0;

	template <typename T>
	static T* get_pointer(void* base_address, int64_t offset) {
		return reinterpret_cast<T*>(static_cast<uint8_t*>(base_address) + offset);
	}

	template <typename T>
	static T* get_pointer(uint64_t base_address, int64_t offset) {
		return reinterpret_cast<T*>(static_cast<uint8_t*>(base_address) + offset);
	}

	static int buffer_state_offset(int rank) {
		return offset_of_send_buffer_state(rank, 0);
	}

	static int offset_of_send_buffer_state(int rank, int idx) {
		return rank * MAX_FLYING_REQ * 2 + idx;
	}

	static int get_recv_buffer_state_offset(int rank, int idx) {
		return rank * MAX_FLYING_REQ * 2 + MAX_FLYING_REQ + idx;
	}

	uint64_t offset_from_pointer(void* pionter, int memory_id) const {
		return ((const uint8_t*)pionter - (const uint8_t*)rdma_buffer_pointers_[memory_id]);
	}

	uint64_t local_address_from_pointer(void* pionter, int memory_id) const {
		return local_address_table_[memory_id] +
				offset_from_pointer(pionter, memory_id);
	}
protected:
	void* fix_system_memory(int64_t* data_mem_size) {
		if(fix_system_memory_ == false) {
			fix_system_memory_ = true;
			*data_mem_size = RDMA_BUF_SIZE - system_rdma_mem_size_;
			return get_pointer<void>(
					rdma_buffer_pointers_[0], system_rdma_mem_size_);
		}
		*data_mem_size = 0;
		return NULL;
	}
	void* allocate_new_rdma_buffer(int* memory_id) {
		if(num_free_memory_ == 0) {
			throw_exception("Out of memory");
		}
		std::pop_heap(free_memory_ids_, free_memory_ids_ + num_free_memory_);
		int next_memory_id = free_memory_ids_[--num_free_memory_];
		allocate_rdma_buffer(next_memory_id);
		*memory_id = next_memory_id;
		return rdma_buffer_pointers_[next_memory_id];
	}

private:
	std::vector<InternalCommunicator> internal_communicators_;
	InternalCommunicator* c_;
	MPI_Group world_group_;

	//! pid -> index for the info
	RankMap proc_index_map_;
	std::vector<int> proc_info_;

	// these are maintained by grow_remote_address_table()
	uint64_t* remote_address_table_;
	int num_procs_;
	int num_address_per_proc_;

	// local RDMA buffers
	void* rdma_buffer_pointers_[MAX_RDMA_BUFFER];
	int free_memory_ids_[MAX_RDMA_BUFFER];
	int num_free_memory_;
	uint64_t local_address_table_[MAX_RDMA_BUFFER];

	bool fix_system_memory_;
	int system_rdma_mem_size_;

	void initialize_rdma_buffer() {
		if(rdma_buffer_pointers_[0] == NULL) {
			allocate_rdma_buffer(0);
			num_free_memory_ = MAX_RDMA_BUFFER-1;
			for(int i = 0; i < num_free_memory_; ++i) {
				free_memory_ids_[i] = i+1;
			}
			std::make_heap(free_memory_ids_, free_memory_ids_ + num_free_memory_);
		}
	}

	void allocate_rdma_buffer(int memory_id) {
		void*& pointer = rdma_buffer_pointers_[memory_id];
		assert (pointer == NULL);
		pointer = page_aligned_xmalloc(RDMA_BUF_SIZE);
		uint64_t dma_address = FJMPI_Rdma_reg_mem(memory_id, pointer, RDMA_BUF_SIZE);
		if(dma_address == FJMPI_RDMA_ERROR) {
			throw_exception("error on FJMPI_Rdma_reg_mem");
		}
		local_address_table_[memory_id] = dma_address;
	}

	uint64_t get_remote_address(int proc_index, int memory_id, int64_t offset) {
		int new_num_entry = num_address_per_proc_;
		bool need_to_grow = false;
		// at first, grow the table if needed
		if(memory_id >= num_address_per_proc_) {
			do {
				new_num_entry = std::max<int>(INITIAL_RADDR_TABLE_SIZE, num_address_per_proc_*2);
			} while(memory_id >= new_num_entry);
			need_to_grow = true;
		}
		if(proc_index >= num_procs_) {
			assert (proc_index < proc_info_.size());
			need_to_grow = true;
		}
		if(need_to_grow) {
			grow_remote_address_table(proc_info_.size(), new_num_entry);
		}
		assert(memory_id < num_address_per_proc_);
		assert(remote_address_table_ != NULL);
		uint64_t& address = remote_address_table_[proc_index*num_address_per_proc_ + memory_id];
		if(address == 0) {
			// we have not stored this address -> get the address
			address = FJMPI_Rdma_get_remote_addr(proc_info_[proc_index], memory_id);
			if(address == FJMPI_RDMA_ERROR) {
				throw_exception("buffer is not registered on the remote host");
			}
		}
		return address + offset;
	}

	void grow_remote_address_table(int num_procs, int num_address_per_proc) {
		assert(num_procs > 0);
		uint64_t* new_table = (uint64_t*)cache_aligned_xcalloc(
				num_procs*num_address_per_proc*sizeof(uint64_t));
		// copy to the new table
		if(remote_address_table_ != NULL) {
			for(int p = 0; p < num_procs_; ++p) {
				for(int i = 0; i < num_address_per_proc_; ++i) {
					new_table[p*num_address_per_proc + i] =
							remote_address_table_[p*num_address_per_proc_ + i];
				}
			}
			free(remote_address_table_); remote_address_table_ = NULL;
		}
		remote_address_table_ = new_table;
		num_procs_ = num_procs;
		num_address_per_proc_ = num_address_per_proc;
	}

	void set_send_buffer(int target) {
		CommTarget& node = c_->proc_info[target];
		while(node.send_queue.size() > 0) {
			int buf_idx = node.send_count % MAX_FLYING_REQ;
			CommunicationBuffer*& comm_buf = node.send_buf[buf_idx];
			if(comm_buf != NULL || c_->send_buffer_state(target, buf_idx).state != READY) {
				break;
			}
			comm_buf = node.send_queue.front();
			node.send_queue.pop_front();
			BufferState& buf_state = c_->send_buffer_state(target, buf_idx);
			int pid = proc_info_[node.proc_index];
			{
				// input RDMA command to send data
				int memory_id = memory_id_of(comm_buf);
				int tag = FIRST_DATA_TAG + buf_idx;
				uint64_t raddr = get_remote_address(node.proc_index, buf_state.memory_id, buf_state.offset);
				uint64_t laddr = local_address_from_pointer(comm_buf->pointer(), memory_id);
				int64_t length = comm_buf->bytes();
				FJMPI_Rdma_put(pid, tag, raddr, laddr, length, node.put_flag);
			}
			{
				// input RDMA command to notify completion
				// make buffer state for the statement of completion
				buf_state.state = COMPLETE;
				buf_state.memory_id = 0;
				buf_state.length = comm_buf->length_;
				int tag = SYSTEM_TAG;
				uint64_t raddr = node.remote_buffer_state +
						sizeof(BufferState) * get_recv_buffer_state_offset(c_->rank, buf_idx);
				uint64_t laddr = local_address_from_pointer(&buf_state, 0);
				FJMPI_Rdma_put(pid, tag, raddr, laddr, sizeof(BufferState), node.put_flag);
			}
			// increment counter
			node.send_count++;
			c_->num_pending_send--;
		}
	}

	void set_recv_buffer(int target) {
		CommTarget& node = c_->proc_info[target];
		while(true) {
			int buf_idx = node.recv_count % MAX_FLYING_REQ;
			CommunicationBuffer*& comm_buf = node.recv_buf[buf_idx];
			if(comm_buf != NULL) {
				break;
			}
			// set new receive buffer
			assert (c_->get_recv_buffer_state(target, buf_idx).state != READY);
			comm_buf = c_->handler->alloc_buffer();
			int memory_id = memory_id_of(comm_buf);
			BufferState& buf_state = c_->get_recv_buffer_state(target, buf_idx);
			buf_state.state = READY;
			buf_state.memory_id = memory_id;
			buf_state.offset = offset_from_pointer(comm_buf->pointer(), memory_id);
			// notify buffer info to the remote process
			int pid = proc_info_[node.proc_index];
			int tag = SYSTEM_TAG;
			uint64_t raddr = node.remote_buffer_state +
					sizeof(BufferState) * offset_of_send_buffer_state(c_->rank, buf_idx);
			uint64_t laddr = local_address_from_pointer(&buf_state, 0);
			FJMPI_Rdma_put(pid, tag, raddr, laddr, sizeof(BufferState), node.put_flag);
			// increment counter
			node.recv_count++;
		}
	}

	void check_recv_completion(int target) {
		CommTarget& node = c_->proc_info[target];
		while(true) {
			int buf_idx = node.recv_complete_count % MAX_FLYING_REQ;
			CommunicationBuffer*& comm_buf = node.recv_buf[buf_idx];
			if(comm_buf == NULL || c_->get_recv_buffer_state(target, buf_idx).state != COMPLETE) {
				break;
			}
			// receive completed
			if(comm_buf->length_ == 0) {
				// received fold completion
				c_->num_recv_active--;
				c_->handler->free_buffer(comm_buf);
			}
			else {
				// set new buffer for the next receiving
				c_->handler->received(comm_buf, target);
			}
			comm_buf = NULL;
			// increment counter
			node.recv_complete_count++;
		}
	}

};

template <typename T>
class FJMpiAlltoallCommunicator
	: public FJMpiAlltoallCommunicatorBase
	, private memory::ConcurrentPool<T>
{
public:
	FJMpiAlltoallCommunicator() : FJMpiAlltoallCommunicatorBase() { }

	memory::Pool<T>* get_allocator() {
		return this;
	}

	virtual int memory_id_of(CommunicationBuffer* comm_buf) {
		return static_cast<MemoryBlock*>(
				static_cast<T*>(comm_buf->base_object()))->memory_id;
	}

protected:
	virtual T* allocate_new() {
		assert (sizeof(MemoryBlock) <= RDMA_BUF_SIZE);
		int64_t data_mem_size;
		void* ptr = this->fix_system_memory(&data_mem_size);
		if(ptr != NULL) {
			T* ret = add_memory_blocks(ptr, data_mem_size, 0);
			if(ret != NULL) {
				return ret;
			}
		}
		int memory_id;
		ptr = this->allocate_new_rdma_buffer(&memory_id);
		return add_memory_blocks(ptr, RDMA_BUF_SIZE, memory_id);
	}

private:
	struct MemoryBlock : public T {
		int memory_id;
		MemoryBlock(int memory_id__) : memory_id(memory_id__) { }
	};

	T* add_memory_blocks(void* ptr, int64_t mem_size, int memory_id) {
		MemoryBlock* buf = (MemoryBlock*)ptr;
		int64_t num_blocks = mem_size / sizeof(MemoryBlock);
		// initialize blocks
		for(int64_t i = 0; i < num_blocks; ++i) {
			new (&buf[i]) MemoryBlock(memory_id);
		}
		// add to free list
		pthread_mutex_lock(&this->thread_sync_);
		for(int64_t i = 1; i < num_blocks; ++i) {
			this->free_list_.push_back(&buf[i]);
		}
		pthread_mutex_unlock(&this->thread_sync_);
		// return the first block
		if(num_blocks > 0) {
			return &buf[0];
		}
		return NULL;
	}
};

#endif // #ifdef ENABLE_FJMPI_RDMA
#endif /* FJMPI_COMM_HPP_ */
