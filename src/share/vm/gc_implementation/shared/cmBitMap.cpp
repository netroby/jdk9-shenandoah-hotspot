
// Concurrent marking bit map wrapper

#include "gc_implementation/shared/cmBitMap.inline.hpp"
#include "utilities/bitMap.inline.hpp"

CMBitMapRO::CMBitMapRO(int shifter) :
  _bm(),
  _shifter(shifter) {
  _bmStartWord = 0;
  _bmWordSize = 0;
}

HeapWord* CMBitMapRO::getNextMarkedWordAddress(const HeapWord* addr,
                                               const HeapWord* limit) const {
  // First we must round addr *up* to a possible object boundary.
  addr = (HeapWord*)align_size_up((intptr_t)addr,
                                  HeapWordSize << _shifter);
  size_t addrOffset = heapWordToOffset(addr);
  if (limit == NULL) {
    limit = _bmStartWord + _bmWordSize;
  }
  size_t limitOffset = heapWordToOffset(limit);
  size_t nextOffset = _bm.get_next_one_offset(addrOffset, limitOffset);
  HeapWord* nextAddr = offsetToHeapWord(nextOffset);
  assert(nextAddr >= addr, "get_next_one postcondition");
  assert(nextAddr == limit || isMarked(nextAddr),
         "get_next_one postcondition");
  return nextAddr;
}

HeapWord* CMBitMapRO::getNextUnmarkedWordAddress(const HeapWord* addr,
                                                 const HeapWord* limit) const {
  size_t addrOffset = heapWordToOffset(addr);
  if (limit == NULL) {
    limit = _bmStartWord + _bmWordSize;
  }
  size_t limitOffset = heapWordToOffset(limit);
  size_t nextOffset = _bm.get_next_zero_offset(addrOffset, limitOffset);
  HeapWord* nextAddr = offsetToHeapWord(nextOffset);
  assert(nextAddr >= addr, "get_next_one postcondition");
  assert(nextAddr == limit || !isMarked(nextAddr),
         "get_next_one postcondition");
  return nextAddr;
}

int CMBitMapRO::heapWordDiffToOffsetDiff(size_t diff) const {
  assert((diff & ((1 << _shifter) - 1)) == 0, "argument check");
  return (int) (diff >> _shifter);
}

#ifndef PRODUCT
bool CMBitMapRO::covers(MemRegion heap_rs) const {
  // assert(_bm.map() == _virtual_space.low(), "map inconsistency");
  assert(((size_t)_bm.size() * ((size_t)1 << _shifter)) == _bmWordSize,
         "size inconsistency");
  return _bmStartWord == (HeapWord*)(heap_rs.start()) &&
         _bmWordSize  == heap_rs.word_size();
}
#endif

void CMBitMapRO::print_on_error(outputStream* st, const char* prefix) const {
  _bm.print_on_error(st, prefix);
}

size_t CMBitMap::compute_size(size_t heap_size) {
  return heap_size / mark_distance();
}

size_t CMBitMap::mark_distance() {
  return MinObjAlignmentInBytes * BitsPerByte;
}

void CMBitMap::initialize(MemRegion heap, MemRegion bitmap) {
  _bmStartWord = heap.start();
  _bmWordSize = heap.word_size();

  _bm.set_map((BitMap::bm_word_t*) bitmap.start());
  _bm.set_size(_bmWordSize >> _shifter);
}

void CMBitMap::clearAll() {
  _bm.clear();
}

void CMBitMap::markRange(MemRegion mr) {
  mr.intersection(MemRegion(_bmStartWord, _bmWordSize));
  assert(!mr.is_empty(), "unexpected empty region");
  assert((offsetToHeapWord(heapWordToOffset(mr.end())) ==
          ((HeapWord *) mr.end())),
         "markRange memory region end is not card aligned");
  // convert address range into offset range
  _bm.at_put_range(heapWordToOffset(mr.start()),
                   heapWordToOffset(mr.end()), true);
}

void CMBitMap::parMarkRange(MemRegion mr) {
  mr.intersection(MemRegion(_bmStartWord, _bmWordSize));
  assert(!mr.is_empty(), "unexpected empty region");
  assert((offsetToHeapWord(heapWordToOffset(mr.end())) ==
          ((HeapWord *) mr.end())),
         "markRange memory region end is not card aligned");
  // convert address range into offset range
  _bm.par_at_put_range(heapWordToOffset(mr.start()),
                   heapWordToOffset(mr.end()), true);
}

void CMBitMap::clearRange(MemRegion mr) {
  mr.intersection(MemRegion(_bmStartWord, _bmWordSize));
  assert(!mr.is_empty(), "unexpected empty region");
  // convert address range into offset range
  _bm.at_put_range(heapWordToOffset(mr.start()),
                   heapWordToOffset(mr.end()), false);
}

MemRegion CMBitMap::getAndClearMarkedRegion(HeapWord* addr,
                                            HeapWord* end_addr) {
  HeapWord* start = getNextMarkedWordAddress(addr);
  start = MIN2(start, end_addr);
  HeapWord* end   = getNextUnmarkedWordAddress(start);
  end = MIN2(end, end_addr);
  assert(start <= end, "Consistency check");
  MemRegion mr(start, end);
  if (!mr.is_empty()) {
    clearRange(mr);
  }
  return mr;
}
