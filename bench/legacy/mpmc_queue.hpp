// The earlier lock-free queues (MPMC.cpp, February 2026), copied unchanged
// except: wrapped in namespace legacy, with #pragma once and <cstdint> added,
// and main() removed.
// Stage 9 measures one hop through MPMC_queue against the new SPSC queue and
// a mutex-protected queue (results/queues.md).
#pragma once

/*
MPMC can be used for Inter thread communication or Inter process communication
Capacity: Bounded (Large pre allocated momeory) cuz dynamic allocation increases latency
serilaization: strict global order
API: single item, atomicity ready
Message size: Fixed
Non blocking for low latency
No data ownership, just value propagation
Minimal latency for all messages(including worst case)
*/

#include <atomic>
#include <iostream>
#include <vector>
#include <cassert>
#include <cstdint>

namespace legacy {

template <typename data_type,
          size_t capacity_= 0,
          typename index_type = uint64_t>
class MPMC_queue
{
private:
    struct Cell
    {
        std::atomic<index_type> seq_num_;
        data_type data_;
    };
    
    std::atomic<index_type> write_index_;
    std::atomic<index_type> read_index_;
    const size_t buffer_mask_;
    std::vector<Cell> buffer_;

public:
    MPMC_queue():buffer_(capacity_),buffer_mask_(capacity_-1)
    {
        assert((capacity_>=2)&&((capacity_&buffer_mask_)==0)); //power of 2
        for(size_t i=0;i<capacity_;i++)
        {
            buffer_[i].seq_num_.store(i,std::memory_order_relaxed); //No ordering guarantees at all
        }
        read_index_.store(0,std::memory_order_relaxed);
        write_index_.store(0,std::memory_order_relaxed);
    }

    bool enqueue(const data_type& data)
    {
        Cell* cell;
        index_type pos=write_index_.load(std::memory_order_relaxed);
        while(1)
        {
            cell=&buffer_[pos&buffer_mask_]; //same as pos%capacity
            index_type seq=cell->seq_num_.load(std::memory_order_acquire); //Ensures reads/writes that came after this load in program order are not moved before it., Used when reading shared data after another thread published it
            intptr_t diff=(intptr_t)seq-(intptr_t)pos;
            if(diff==0)
            {
                if(write_index_.compare_exchange_weak(pos,pos+1,std::memory_order_relaxed))   //If another thread beat us, it reloads pos and we try again.
                    break;
            }
            else if(diff<0)
            {
                return false;        //full
            }
            else
            {
                pos=write_index_.load(std::memory_order_relaxed);
            }
        }
        cell->data_=data;
        cell->seq_num_.store(pos+1,std::memory_order_release);  //Prevents previous writes from being reordered after this store, Ensures all previous writes are committed before this store.
        return true;
    }

    bool dequeue(data_type& data)
    {
        Cell* cell;
        index_type pos=read_index_.load(std::memory_order_relaxed);
        while(1)
        {
            cell=&buffer_[pos&buffer_mask_];
            index_type seq=cell->seq_num_.load(std::memory_order_acquire); //Prevents later reads/writes from being reordered before this load, Used when reading shared data after another thread published it
            intptr_t diff=(intptr_t)seq-(intptr_t)(pos+1);
            if(diff==0)
            {
                if(read_index_.compare_exchange_weak(pos,pos+1,std::memory_order_relaxed))   //If another thread beat us, it reloads pos and we try again.
                    break;
            }
            else if(diff<0)     //empty
            {
                return false;
            }
            else
            {
                pos=read_index_.load(std::memory_order_relaxed);
            }
        }
        data=cell->data_;
        cell->seq_num_.store(pos+(index_type)capacity_,std::memory_order_release);
        return true;   
    }
};

template <typename data_type,
          size_t capacity_=0,
          typename index_type=uint64_t>
class SPSC_queue
{
private:
    alignas(64) std::atomic<index_type> read_index_;
    alignas(64) std::atomic<index_type> write_index_;
    const size_t buffer_mask_;
    std::vector<data_type> buffer_;

public:
    SPSC_queue(): buffer_(capacity_),buffer_mask_(capacity_-1)
    {
        assert((capacity_>=2)&&((capacity_&buffer_mask_)==0));
        read_index_.store(0,std::memory_order_relaxed);
        write_index_.store(0,std::memory_order_relaxed);
    }

    bool enqueue(const data_type& data)
    {
        index_type write= write_index_.load(std::memory_order_relaxed);
        index_type read=read_index_.load(std::memory_order_acquire);
        intptr_t diff=(intptr_t)(write+1)-(intptr_t)read;
        if(diff>=(intptr_t)capacity_) return false;
        buffer_[write&buffer_mask_]=data;
        write_index_.store(write+1,std::memory_order_release);
        return true;
    }

    bool dequeue(data_type& data)
    {
        index_type write= write_index_.load(std::memory_order_acquire);
        index_type read=read_index_.load(std::memory_order_relaxed);
        if(read==write) return false;
        data=buffer_[read&buffer_mask_];
        read_index_.store(read+1,std::memory_order_release);
        return true;
    }
};

}  // namespace legacy
