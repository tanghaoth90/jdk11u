
#ifndef SHARE_GC_Z_ZPAGECACHEBALANCE_HPP
#define SHARE_GC_Z_ZPAGECACHEBALANCE_HPP

#include "gc/z/zPageAllocator.hpp"
#include "gc/z/zPageCache.inline.hpp"

class ZPageAllocator;

// convert cached pages into the other type in order to satisfy the need of relocation or mutator's allocation rate
class ZPageCacheBalance {
private:
  ZPageAllocator* _page_allocator;
  ZPageTable* _pagetable;
  ZPageCache* _cache;
  bool _before_relocation;
  size_t _small_selected_to, _medium_selected_to;

  // balance page cache: (_available_small, _available_medium) -> (_target_small, _target_medium)
  size_t _available_small, _available_medium; // number of available small/medium cached pages
  size_t _target_small, _target_medium;       // number of target small/medium cached pages

  // conversion: loaner pages -> debtor pages       (transform loaner to debtor)
  //               (small)         (medium)         if _available_small > _target_small
  //              (medium)          (small)         if _available_small < _target_small
  size_t _loaner_count, _debtor_count;
  uint8_t _loaner_type, _debtor_type;
  ZList<ZPage> _loaner_list, _debtor_list;

  Ticks _ticks_start;

public:
  ZPageCacheBalance(ZPageAllocator* page_allocator, ZPageTable* _pagetable, bool before_relocation, size_t small_selected_to, size_t medium_selected_to);
  ~ZPageCacheBalance();
  void balance();

private:
  bool need_to_balance();
  void unmap();
  void remap();

  void initialize_page_count();
  bool determine_balance_necessity();
  void calculate_loaner_and_debtor();

  void unmap_pages();
  void free_physical_memory();
  void create_pages_for_debtor();
  void map_pages();
  void insert_pages_to_page_cache();

  size_t calculate_optimal_small();
  size_t calculate_optimal_medium();
  size_t calculate_minimal_small();
  size_t calculate_minimal_medium();
  size_t calculate_maximal_small_for_medium(size_t medium);
  size_t calculate_maximal_medium_for_small(size_t small);

  size_t available_page_cache_size();
  size_t calculate_total_size(size_t small_count, size_t medium_count);
};

#endif // SHARE_GC_Z_ZPAGECACHEBALANCE_HPP
