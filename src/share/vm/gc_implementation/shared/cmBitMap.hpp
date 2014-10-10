
#ifndef SHARE_VM_GC_IMPLEMENTATION_SHARED_CMBITMAP_HPP
#define SHARE_VM_GC_IMPLEMENTATION_SHARED_CMBITMAP_HPP

#include "memory/memRegion.hpp"
#include "oops/oop.inline.hpp"
#include "utilities/bitMap.hpp"
#include "utilities/globalDefinitions.hpp"

// A generic CM bit map.  This is essentially a wrapper around the BitMap
// class, with one bit per (1<<_shifter) HeapWords.

class CMBitMapRO VALUE_OBJ_CLASS_SPEC {
 protected:
  HeapWord* _bmStartWord;      // base address of range covered by map
  size_t    _bmWordSize;       // map size (in #HeapWords covered)
  const int _shifter;          // map to char or bit
  BitMap    _bm;               // the bit map itself

 public:
  // constructor
  CMBitMapRO(int shifter);

  enum { do_yield = true };

  // inquiries
  HeapWord* startWord()   const { return _bmStartWord; }
  size_t    sizeInWords() const { return _bmWordSize;  }
  // the following is one past the last word in space
  HeapWord* endWord()     const { return _bmStartWord + _bmWordSize; }

  // read marks

  bool isMarked(HeapWord* addr) const {
    assert(_bmStartWord <= addr && addr < (_bmStartWord + _bmWordSize),
           "outside underlying space?");
    return _bm.at(heapWordToOffset(addr));
  }

  // iteration
  inline bool iterate(BitMapClosure* cl, MemRegion mr);
  inline bool iterate(BitMapClosure* cl);

  // Return the address corresponding to the next marked bit at or after
  // "addr", and before "limit", if "limit" is non-NULL.  If there is no
  // such bit, returns "limit" if that is non-NULL, or else "endWord()".
  HeapWord* getNextMarkedWordAddress(const HeapWord* addr,
                                     const HeapWord* limit = NULL) const;
  // Return the address corresponding to the next unmarked bit at or after
  // "addr", and before "limit", if "limit" is non-NULL.  If there is no
  // such bit, returns "limit" if that is non-NULL, or else "endWord()".
  HeapWord* getNextUnmarkedWordAddress(const HeapWord* addr,
                                       const HeapWord* limit = NULL) const;

  // conversion utilities
  HeapWord* offsetToHeapWord(size_t offset) const {
    return _bmStartWord + (offset << _shifter);
  }
  size_t heapWordToOffset(const HeapWord* addr) const {
    return pointer_delta(addr, _bmStartWord) >> _shifter;
  }
  int heapWordDiffToOffsetDiff(size_t diff) const;

  // The argument addr should be the start address of a valid object
  HeapWord* nextObject(HeapWord* addr) {
    oop obj = (oop) addr;
    HeapWord* res =  addr + obj->size();
    assert(offsetToHeapWord(heapWordToOffset(res)) == res, "sanity");
    return res;
  }

  void print_on_error(outputStream* st, const char* prefix) const;

  // debugging
  NOT_PRODUCT(bool covers(MemRegion rs) const;)
};

class CMBitMap : public CMBitMapRO {

 public:
  static size_t compute_size(size_t heap_size);
  // Returns the amount of bytes on the heap between two marks in the bitmap.
  static size_t mark_distance();

  CMBitMap() : CMBitMapRO(LogMinObjAlignment) {}

  // Initializes the underlying BitMap to cover the given area.
  void initialize(MemRegion heap, MemRegion bitmap);

  // Write marks.
  inline void mark(HeapWord* addr);
  inline void clear(HeapWord* addr);
  inline bool parMark(HeapWord* addr);
  inline bool parClear(HeapWord* addr);

  void markRange(MemRegion mr);
  void parMarkRange(MemRegion mr);
  void clearRange(MemRegion mr);

  // Starting at the bit corresponding to "addr" (inclusive), find the next
  // "1" bit, if any.  This bit starts some run of consecutive "1"'s; find
  // the end of this run (stopping at "end_addr").  Return the MemRegion
  // covering from the start of the region corresponding to the first bit
  // of the run to the end of the region corresponding to the last bit of
  // the run.  If there is no "1" bit at or after "addr", return an empty
  // MemRegion.
  MemRegion getAndClearMarkedRegion(HeapWord* addr, HeapWord* end_addr);

  // Clear the whole mark bitmap.
  void clearAll();
};

#endif // SHARE_VM_GC_IMPLEMENTATION_SHARED_CMBITMAP_HPP
