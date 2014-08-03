/** heap building blocks -*- C++ -*-
 * @file
 * @section License
 *
 * Galois, a framework to exploit amorphous data-parallelism in irregular
 * programs.
 *
 * Copyright (C) 2013, The University of Texas at Austin. All rights reserved.
 * UNIVERSITY EXPRESSLY DISCLAIMS ANY AND ALL WARRANTIES CONCERNING THIS
 * SOFTWARE AND DOCUMENTATION, INCLUDING ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR ANY PARTICULAR PURPOSE, NON-INFRINGEMENT AND WARRANTIES OF
 * PERFORMANCE, AND ANY WARRANTY THAT MIGHT OTHERWISE ARISE FROM COURSE OF
 * DEALING OR USAGE OF TRADE.  NO WARRANTY IS EITHER EXPRESS OR IMPLIED WITH
 * RESPECT TO THE USE OF THE SOFTWARE OR DOCUMENTATION. Under no circumstances
 * shall University be liable for incidental, special, indirect, direct or
 * consequential damages or loss of profits, interruption of business, or
 * related expenses which may arise from use of Software or Documentation,
 * including but not limited to those resulting from defects in Software and/or
 * Documentation, or loss or inaccuracy of data of any kind.
 *
 * @section Description
 *
 * Strongly inspired by heap layers:
 *  http://www.heaplayers.org/
 * FSB is modified from:
 *  http://warp.povusers.org/FSBAllocator/
 *
 * @author Andrew Lenharth <andrewl@lenharth.org>
 */
#ifndef GALOIS_RUNTIME_MEM_H
#define GALOIS_RUNTIME_MEM_H

#include "Galois/Runtime/PerThreadStorage.h"
#include "Galois/Runtime/ll/SimpleLock.h"
#include "Galois/Runtime/ll/PtrLock.h"
#include "Galois/Runtime/ll/CacheLineStorage.h"

#include <boost/utility.hpp>
#include <cstdlib>
#include <cstring>
#include <map>
#include <list>
#include <cstddef>

#include <memory.h>

namespace Galois {
namespace Runtime {
//! Memory management functionality.
namespace MM {

extern size_t pageSize;
const size_t hugePageSize = 2*1024*1024;

void* pageAlloc();
void pageFree(void*);
//! Preallocate numpages large pages for each thread
void pagePreAlloc(int numpages);
//! Forces the given block to be paged into physical memory
void pageIn(void *buf, size_t len, size_t stride);
//! Forces the given readonly block to be paged into physical memory
void pageInReadOnly(void *buf, size_t len, size_t stride);

//! Returns total large pages allocated by Galois memory management subsystem
int numPageAllocTotal();
//! Returns total large pages allocated for thread by Galois memory management subsystem
int numPageAllocForThread(unsigned tid);

//! Returns total small pages allocated by OS on a NUMA node
int numNumaAllocForNode(unsigned nodeid);
//! Returns number of NUMA nodes on machine
int numNumaNodes();

/**
 * Allocates memory interleaved across NUMA nodes. 
 * 
 * If full, allocate across all NUMA nodes; otherwise,
 * allocate across NUMA nodes corresponding to active
 * threads.
 */
void* largeInterleavedAlloc(size_t bytes, bool full = true);
//! Frees memory allocated by {@link largeInterleavedAlloc()}
void largeInterleavedFree(void* mem, size_t bytes);

//! Allocates a large block of memory
void* largeAlloc(size_t bytes, bool preFault = true);
//! Frees memory allocated by {@link largeAlloc()}
void largeFree(void* mem, size_t bytes);

//! Print lines from /proc/pid/numa_maps that contain at least n (non-huge) pages
void printInterleavedStats(int minPages = 16*1024);

//! [Example Third Party Allocator]
class MallocHeap {
public:
  enum { AllocSize = 0 };
  void* allocate(size_t size) {
    return malloc(size);
  }
  void deallocate(void* ptr, size_t len) {
    free(ptr);
  }
};
//! [Example Third Party Allocator]


//! Per-thread heaps using Galois thread aware construct
template<class LocalHeap>
class ThreadAwarePrivateHeap {
  PerThreadStorage<LocalHeap> heaps;

public:
  enum { AllocSize = LocalHeap::AllocSize };

  ThreadAwarePrivateHeap() {}
  ~ThreadAwarePrivateHeap() {
    clear();
  }

  template<typename... Args>
  inline void* allocate(size_t size, Args&&... args) {
    return heaps.getLocal()->allocate(size, std::forward<Args>(args)...);
  }

  inline void deallocate(void* ptr, size_t len) {
    heaps.getLocal()->deallocate(ptr, len);
  }

  void clear() {
    for (unsigned int i = 0; i < heaps.size(); i++)
      heaps.getRemote(i)->clear();
  }
};

//! Apply a lock to a heap
template<class RealHeap>
class LockedHeap : public RealHeap {
  LL::SimpleLock lock;
public :
  enum { AllocSize = RealHeap::AllocSize };

  inline void* allocate(size_t size) {
    lock.lock();
    void* retval = RealHeap::allocate(size);
    lock.unlock();
    return retval;
  }
  
  inline void deallocate(void* ptr, size_t len) {
    lock.lock();
    RealHeap::deallocate(ptr, len);
    lock.unlock();
  }
};

template<typename SourceHeap>
class ZeroOut : public SourceHeap {
public:
  enum { AllocSize = SourceHeap::AllocSize } ;
  inline void* allocate(size_t size) {
    void* retval = SourceHeap::allocate(size);
    memset(retval, 0, size);
    return retval;
  }

  inline void deallocate(void* ptr, size_t len) {
    SourceHeap::deallocate(ptr, len);
  }
};

//! Add a header to objects
template<typename Header, typename SourceHeap>
class AddHeader : public SourceHeap {
  enum { offset = (sizeof(Header) + (sizeof(double) - 1)) & ~(sizeof(double) - 1) };

public:
  inline void* allocate(size_t size) {
    //First increase the size of the header to be aligned to a double
    void* ptr = SourceHeap::allocate(size + offset);
    //Now return the offseted pointer
    return (char*)ptr + offset;
  }
  
  inline void deallocate(void* ptr, size_t len) {
    SourceHeap::deallocate(getHeader(ptr), len + offset);
  }

  inline static Header* getHeader(void* ptr) {
    return (Header*)((char*)ptr - offset);
  }
};

//! Allow looking up parent heap pointers
template<class SourceHeap>
class OwnerTaggedHeap : public AddHeader<void*, SourceHeap> {
  typedef AddHeader<OwnerTaggedHeap*, SourceHeap> Src;
public:
  inline void* allocate(size_t size) {
    void* retval = Src::allocate(size);
    *(Src::getHeader(retval)) = this;
    return retval;
  }

  inline void deallocate(void* ptr, size_t len) {
    assert(*(Src::getHeader(ptr)) == this);
    Src::deallocate(ptr, len);
  }

  inline static OwnerTaggedHeap* owner(void* ptr) {
    return *(OwnerTaggedHeap**)Src::getHeader(ptr);
  }
};

//! Maintain a freelist
template<class SourceHeap>
class FreeListHeap : public SourceHeap {
  struct FreeNode {
    FreeNode* next;
  };
  FreeNode* head;

public:
  enum { AllocSize = SourceHeap::AllocSize };

  void clear() {
    while (head) {
      FreeNode* N = head;
      head = N->next;
      SourceHeap::deallocate(N, 1);
    }
  }

  FreeListHeap() : head(0) {}
  ~FreeListHeap() {
    clear();
  }

  inline void* allocate(size_t size) {
    if (head) {
      void* ptr = head;
      head = head->next;
      return ptr;
    }
    return SourceHeap::allocate(size);
  }

  inline void deallocate(void* ptr, size_t len) {
    if (!ptr) return;
    assert((uintptr_t)ptr > 0x100);
    FreeNode* NH = (FreeNode*)ptr;
    NH->next = head;
    head = NH;
  }
};

//! Maintain a freelist using a lock which doesn't cover SourceHeap
template<class SourceHeap>
class SelfLockFreeListHeap : public SourceHeap {
  struct FreeNode {
    FreeNode* next;
  };
  FreeNode* head;

public:
  enum { AllocSize = SourceHeap::AllocSize };

  void clear() {
    FreeNode* h = 0;
    do {
      h = head;
    } while (!__sync_bool_compare_and_swap(&head, h, 0));
    while (h) {
      FreeNode* N = h;
      h = N->next;
      SourceHeap::deallocate(N, 1);
    }
  }

  SelfLockFreeListHeap() : head(0) {}
  ~SelfLockFreeListHeap() {
    clear();
  }

  inline void* allocate(size_t size) {
    static LL::SimpleLock lock;

    lock.lock();
    FreeNode* OH = 0;
    FreeNode* NH = 0;
    do {
      OH = head;
      if (!OH) {
	lock.unlock();
	return SourceHeap::allocate(size);
      }
      NH = OH->next; //The lock protects this line
    } while (!__sync_bool_compare_and_swap(&head, OH, NH));
    lock.unlock();
    assert(OH);
    return (void*)OH;
  }

  inline void deallocate(void* ptr, size_t len) {
    if (!ptr) return;
    FreeNode* OH;
    FreeNode* NH;
    do {
      OH = head;
      NH = (FreeNode*)ptr;
      NH->next = OH;
    } while (!__sync_bool_compare_and_swap(&head, OH, NH));
  }

};

template<unsigned ElemSize, typename SourceHeap>
class BlockAlloc : public SourceHeap {

  struct TyEq {
    double data[((ElemSize + sizeof(double) - 1) & ~(sizeof(double) - 1))/sizeof(double)];
  };

  struct Block_basic {
    union {
      Block_basic* next;
      double dummy;
    };
    TyEq data[1];
  };

  enum {BytesLeft = (SourceHeap::AllocSize - sizeof(Block_basic)),
	BytesLeftR = BytesLeft & ~(sizeof(double) - 1),
	FitLeft = BytesLeftR / sizeof(TyEq[1]),
	TotalFit = FitLeft + 1
  };

  struct Block {
    union {
      Block* next;
      double dummy;
    };
    TyEq data[TotalFit];
  };

  Block* head;
  int headIndex;

  void refill() {
    void* P = SourceHeap::allocate(SourceHeap::AllocSize);
    Block* BP = (Block*)P;
    BP->next = head;
    head = BP;
    headIndex = 0;
  }
public:
  enum { AllocSize = ElemSize };

  void clear() {
    while(head) {
      Block* B = head;
      head = B->next;
      SourceHeap::deallocate(B, SourceHeap::AllocSize);
    }
  }

  BlockAlloc() :SourceHeap(), head(0), headIndex(0) {
    assert(sizeof(Block) <= SourceHeap::AllocSize);
  }

  ~BlockAlloc() {
    clear();
  }

  inline void* allocate(size_t size) {
    assert(size == ElemSize);
    if (!head || headIndex == TotalFit)
      refill();
    return &head->data[headIndex++];
  }

  inline void deallocate(void* ptr, size_t len) {}
};

//! This implements a bump pointer though chunks of memory
template<typename SourceHeap>
class SimpleBumpPtr : public SourceHeap {

  struct Block {
    union {
      Block* next;
      double dummy; // for alignment
    };
  };

  Block* head;
  int offset;

  void refill() {
    void* P = SourceHeap::allocate(SourceHeap::AllocSize);
    Block* BP = (Block*)P;
    BP->next = head;
    head = BP;
    offset = sizeof(Block);
  }
public:
  enum { AllocSize = 0 };

  SimpleBumpPtr(): SourceHeap(), head(0), offset(0) {}
  ~SimpleBumpPtr() {
    clear();
  }

  void clear() {
    while (head) {
      Block* B = head;
      head = B->next;
      SourceHeap::deallocate(B, SourceHeap::AllocSize);
    }
  }

  inline void* allocate(size_t size) {
    // Increase to alignment
    size_t alignedSize = (size + sizeof(double) - 1) & ~(sizeof(double) - 1);
    // Check current block
    if (!head || offset + alignedSize > SourceHeap::AllocSize)
      refill();
    if (offset + alignedSize > SourceHeap::AllocSize)
      throw std::bad_alloc();
    char* retval = (char*)head;
    retval += offset;
    offset += alignedSize;
    return retval;
  }

  /**
   * Allocates size bytes but may fail. If so, size < allocated and
   * allocated is the number of bytes allocated in the returned buffer.
   */
  inline void* allocate(size_t size, size_t& allocated) {
    // Increase to alignment
    size_t alignedSize = (size + sizeof(double) - 1) & ~(sizeof(double) - 1);
    if (alignedSize > SourceHeap::AllocSize) {
      alignedSize = SourceHeap::AllocSize;
    }
    // Check current block
    if (!head || offset + alignedSize > SourceHeap::AllocSize) {
      size_t remaining = SourceHeap::AllocSize - offset;
      assert((remaining & (sizeof(double) - 1)) == 0); // should still be aligned
      if (!remaining) {
        refill();
      } else {
        alignedSize = remaining;
      }
    }
    char* retval = (char*)head;
    retval += offset;
    offset += alignedSize;
    allocated = (alignedSize > size) ? size : alignedSize;
    return retval;
  }

  inline void deallocate(void* ptr, size_t len) {}
};

/**
 * This implements a bump pointer though chunks of memory that falls back
 * to malloc if the source heap cannot accommodate an allocation.
 */
template<typename SourceHeap>
class SimpleBumpPtrWithMallocFallback : public SourceHeap {
  struct Block {
    union {
      Block* next;
      double dummy; // for alignment
    };
  };

  Block* head;
  Block* fallbackHead;
  int offset;

  //! Given block of memory P, update head pointer and offset metadata
  void refill(void* P, Block*& h, int* o) {
    Block* BP = (Block*)P;
    BP->next = h;
    h = BP;
    if (o)
      *o = sizeof(Block);
  }
public:
  enum { AllocSize = 0 };

  SimpleBumpPtrWithMallocFallback(): SourceHeap(), head(0), fallbackHead(0), offset(0) { }

  ~SimpleBumpPtrWithMallocFallback() {
    clear();
  }

  void clear() {
    while (head) {
      Block* B = head;
      head = B->next;
      SourceHeap::deallocate(B, SourceHeap::AllocSize);
    }
    while (fallbackHead) {
      Block* B = fallbackHead;
      fallbackHead = B->next;
      free(B);
    }
  }

  inline void* allocate(size_t size) {
    // Increase to alignment
    size_t alignedSize = (size + sizeof(double) - 1) & ~(sizeof(double) - 1);
    if (sizeof(Block) + alignedSize > SourceHeap::AllocSize) {
      void* p = malloc(alignedSize + sizeof(Block));
      refill(p, fallbackHead, NULL);
      return (char*)p + sizeof(Block);
    }
    // Check current block
    if (!head || offset + alignedSize > SourceHeap::AllocSize)
      refill(SourceHeap::allocate(SourceHeap::AllocSize), head, &offset);
    char* retval = (char*)head;
    retval += offset;
    offset += alignedSize;
    return retval;
  }

  inline void deallocate(void* ptr, size_t len) {}
};

//! This is the base source of memory for all allocators.
//! It maintains a freelist of hunks acquired from the system
class SystemBaseAlloc {
public:
  enum { AllocSize = hugePageSize };

  SystemBaseAlloc();
  ~SystemBaseAlloc();

  inline void* allocate(size_t size) {
    return pageAlloc();
  }

  inline void deallocate(void* ptr, size_t len) {
    pageFree(ptr);
  }
};

#ifdef GALOIS_FORCE_STANDALONE
class SizedAllocatorFactory: private boost::noncopyable {
public:
  typedef MallocHeap SizedAlloc;

  static SizedAlloc* getAllocatorForSize(const size_t) {
    return &alloc;
  }

private:
  static SizedAlloc alloc;
};
#else
class SizedAllocatorFactory: private boost::noncopyable {
public:
//! [FixedSizeAllocator example]
  typedef ThreadAwarePrivateHeap<
    FreeListHeap<SimpleBumpPtr<SystemBaseAlloc> > > SizedAlloc;
//! [FixedSizeAllocator example]

  static SizedAlloc* getAllocatorForSize(const size_t);

private:
  typedef std::map<size_t, SizedAlloc*> AllocatorsMap;
  static SizedAllocatorFactory* getInstance();
  static LL::PtrLock<SizedAllocatorFactory, true> instance;
  static __thread AllocatorsMap* localAllocators;
  AllocatorsMap allocators;
  std::list<AllocatorsMap*> allLocalAllocators;
  LL::SimpleLock lock;

  SizedAllocatorFactory();
  ~SizedAllocatorFactory();

  SizedAlloc* getAllocForSize(const size_t);
};
#endif

/**
 * Scalable variable-size allocations.
 *
 * Slight misnomer as this doesn't support allocations greater than a page.
 * Users should call {@link allocate(size_t, size_t&)} multiple times to split
 * large allocations over multiple pages.
 */
class VariableSizeAllocator: public ThreadAwarePrivateHeap<SimpleBumpPtr<SystemBaseAlloc>> {

};

//! Main scalable allocator in Galois
class FixedSizeAllocator {
  SizedAllocatorFactory::SizedAlloc* alloc;
public:
  FixedSizeAllocator(size_t sz) {
    alloc = SizedAllocatorFactory::getAllocatorForSize(sz);
  }

  inline void* allocate(size_t sz) {
    return alloc->allocate(sz);
  }

  inline void deallocate(void* ptr, size_t len) {
    alloc->deallocate(ptr, len);
  }

  inline bool operator!=(const FixedSizeAllocator& rhs) const {
    return alloc != rhs.alloc;
  }
  
  inline bool operator==(const FixedSizeAllocator& rhs) const {
    return alloc == rhs.alloc;
  }
};

////////////////////////////////////////////////////////////////////////////////
// Now adapt to standard std allocators
////////////////////////////////////////////////////////////////////////////////

//!A fixed size block allocator
template<typename Ty>
class FSBGaloisAllocator;

template<>
class FSBGaloisAllocator<void> {
public:
  typedef size_t size_type;
  typedef ptrdiff_t difference_type;
  typedef void* pointer;
  typedef const void* const_pointer;
  typedef void value_type;

  template<typename Other>
  struct rebind { typedef FSBGaloisAllocator<Other> other; };
};

template<typename Ty>
class FSBGaloisAllocator {
  inline void destruct(char*) const { }
  inline void destruct(wchar_t*) const { }
  template<typename T> inline void destruct(T* t) const { t->~T(); }

  FixedSizeAllocator Alloc;

public:
  typedef size_t size_type;
  typedef ptrdiff_t difference_type;
  typedef Ty *pointer;
  typedef const Ty *const_pointer;
  typedef Ty& reference;
  typedef const Ty& const_reference;
  typedef Ty value_type;
  
  template<class Other>
  struct rebind { typedef FSBGaloisAllocator<Other> other; };

  FSBGaloisAllocator() throw(): Alloc(sizeof(Ty)) {}
  template <class U> FSBGaloisAllocator(const FSBGaloisAllocator<U>&) throw(): Alloc(sizeof(Ty)) {}

  inline pointer address(reference val) const { return &val; }
  inline const_pointer address(const_reference val) const { return &val; }

  pointer allocate(size_type size) {
    if (size > max_size())
      throw std::bad_alloc();
    return static_cast<pointer>(Alloc.allocate(sizeof(Ty)));
  }
  
  void deallocate(pointer ptr, size_type len) {
    Alloc.deallocate(ptr, len);
  }
  
  template<class U, class... Args>
  inline void construct(U* p, Args&&... args ) const {
    ::new((void*)p) U(std::forward<Args>(args)...);
  }
  
  inline void destroy(pointer ptr) const {
    destruct(ptr);
  }

  size_type max_size() const throw() { return 1; }

  template<typename T1>
  inline bool operator!=(const FSBGaloisAllocator<T1>& rhs) const {
    return Alloc != rhs.Alloc;
  }

  template<typename T1>
  inline bool operator==(const FSBGaloisAllocator<T1>& rhs) const {
    return Alloc == rhs.Alloc;
  }
};

//template<typename T1,typename T2>
//bool operator!=(const FSBGaloisAllocator<T1>& lhs, const FSBGaloisAllocator<T2>& rhs) {
//  return lhs.Alloc != rhs.Alloc;
//}

//!Keep a reference to an external allocator
template<typename Ty, typename AllocTy>
class ExternRefGaloisAllocator;

template<typename AllocTy>
class ExternRefGaloisAllocator<void,AllocTy> {
public:
  typedef size_t size_type;
  typedef ptrdiff_t difference_type;
  typedef void* pointer;
  typedef const void* const_pointer;
  typedef void value_type;

  template<typename Other>
  struct rebind { typedef ExternRefGaloisAllocator<Other,AllocTy> other; };
};

template<typename Ty, typename AllocTy>
class ExternRefGaloisAllocator {
  inline void destruct(char*) const {}
  inline void destruct(wchar_t*) const { }
  template<typename T> inline void destruct(T* t) const { t->~T(); }

public:
  AllocTy* Alloc; // Should be private except that makes copy hard

  typedef size_t size_type;
  typedef ptrdiff_t difference_type;
  typedef Ty *pointer;
  typedef const Ty *const_pointer;
  typedef Ty& reference;
  typedef const Ty& const_reference;
  typedef Ty value_type;
  
  template<class Other>
  struct rebind {
    typedef ExternRefGaloisAllocator<Other, AllocTy> other;
  };

  explicit ExternRefGaloisAllocator(AllocTy* a) throw(): Alloc(a) {}

  template<class T1>
  ExternRefGaloisAllocator(const ExternRefGaloisAllocator<T1,AllocTy>& rhs) throw() {
    Alloc = rhs.Alloc;
  }
  
  inline pointer address(reference val) const { return &val; }
  inline const_pointer address(const_reference val) const { return &val; }
  
  pointer allocate(size_type size) {
    if (size > max_size())
      throw std::bad_alloc();
    return static_cast<pointer>(Alloc->allocate(size*sizeof(Ty)));
  }
  
  void deallocate(pointer ptr, size_type len) {
    Alloc->deallocate(ptr, len);
  }
  
  inline void construct(pointer ptr, const_reference val) const {
    new (ptr) Ty(val);
  }

  template<class U, class... Args >
  inline void construct(U* p, Args&&... args ) const {
    ::new((void*)p) U(std::forward<Args>(args)...);
  }
  
  void destroy(pointer ptr) const {
    destruct(ptr);
  }
  
  size_type max_size() const throw() { return size_t(-1)/sizeof(Ty); }

  template<typename T1,typename A1>
  bool operator!=(const ExternRefGaloisAllocator<T1,A1>& rhs) const {
    return Alloc != rhs.Alloc;
  }
};

struct SerialNumaHeap {
  void* allocate (size_t len) {
    return largeInterleavedAlloc (len, true);
  }

  void deallocate (void* ptr, size_t len) {
    largeInterleavedFree (ptr, len);
  }
};

template <typename T>
class SerialNumaAllocator: public ExternRefGaloisAllocator<T, SerialNumaHeap> {
  using Super = ExternRefGaloisAllocator<T, SerialNumaHeap>;
  SerialNumaHeap heap;

public:
  SerialNumaAllocator (): Super (&heap) {}
};


} // end namespace MM
} // end namespace Runtime
} // end namespace Galois

#endif
