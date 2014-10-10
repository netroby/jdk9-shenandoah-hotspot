
#ifndef SHARE_VM_GC_IMPLEMENTATION_SHARED_CMBITMAP_INLINE_HPP
#define SHARE_VM_GC_IMPLEMENTATION_SHARED_CMBITMAP_INLINE_HPP

#include "gc_implementation/shared/cmBitMap.hpp"
#include "utilities/bitMap.inline.hpp"

inline bool CMBitMapRO::iterate(BitMapClosure* cl, MemRegion mr) {
  HeapWord* start_addr = MAX2(startWord(), mr.start());
  HeapWord* end_addr = MIN2(endWord(), mr.end());

  if (end_addr > start_addr) {
    // Right-open interval [start-offset, end-offset).
    BitMap::idx_t start_offset = heapWordToOffset(start_addr);
    BitMap::idx_t end_offset = heapWordToOffset(end_addr);

    start_offset = _bm.get_next_one_offset(start_offset, end_offset);
    while (start_offset < end_offset) {
      if (!cl->do_bit(start_offset)) {
        return false;
      }
      HeapWord* next_addr = MIN2(nextObject(offsetToHeapWord(start_offset)), end_addr);
      BitMap::idx_t next_offset = heapWordToOffset(next_addr);
      start_offset = _bm.get_next_one_offset(next_offset, end_offset);
    }
  }
  return true;
}

inline bool CMBitMapRO::iterate(BitMapClosure* cl) {
  MemRegion mr(startWord(), sizeInWords());
  return iterate(cl, mr);
}

#define check_mark(addr)                                                       \
  assert(_bmStartWord <= (addr) && (addr) < (_bmStartWord + _bmWordSize),      \
         "outside underlying space?");                                         \
  /* assert(G1CollectedHeap::heap()->is_in_exact(addr),                  \
         err_msg("Trying to access not available bitmap "PTR_FORMAT            \
                 " corresponding to "PTR_FORMAT" (%u)",                        \
                 p2i(this), p2i(addr), G1CollectedHeap::heap()->addr_to_region(addr))); */

inline void CMBitMap::mark(HeapWord* addr) {
  check_mark(addr);
  _bm.set_bit(heapWordToOffset(addr));
}

inline void CMBitMap::clear(HeapWord* addr) {
  check_mark(addr);
  _bm.clear_bit(heapWordToOffset(addr));
}

inline bool CMBitMap::parMark(HeapWord* addr) {
  check_mark(addr);
  return _bm.par_set_bit(heapWordToOffset(addr));
}

inline bool CMBitMap::parClear(HeapWord* addr) {
  check_mark(addr);
  return _bm.par_clear_bit(heapWordToOffset(addr));
}

#undef check_mark

#endif // SHARE_VM_GC_IMPLEMENTATION_SHARED_CMBITMAP_INLINE_HPP
