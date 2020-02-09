
#include "precompiled.hpp"
#include "gc/z/zHeap.inline.hpp"
#include "gc/z/zList.inline.hpp"
#include "gc/z/zLock.inline.hpp"
#include "gc/z/zPageCache.inline.hpp"
#include "gc/z/zPageCacheBalance.hpp"
#include "gc/z/zStat.hpp"

ZPageCacheBalance::ZPageCacheBalance(ZPageAllocator* page_allocator, ZPageTable* pagetable, bool before_relocation, size_t small_selected_to, size_t medium_selected_to):
  _page_allocator(page_allocator),
  _pagetable(pagetable),
  _before_relocation(before_relocation),
  _small_selected_to(small_selected_to),
  _medium_selected_to(medium_selected_to),
  _available_small(0),
  _available_medium(0),
  _target_small(0),
  _target_medium(0),
  _loaner_type(ZPageTypeSmall),
  _debtor_type(ZPageTypeMedium),
  _loaner_count(0),
  _debtor_count(0) {

  assert(ZBalancePageCache, "sanity");
  _cache = &(page_allocator->_cache);
  _ticks_start = Ticks::now();
}

ZPageCacheBalance::~ZPageCacheBalance() {
  double duration = TimeHelper::counter_to_millis((Ticks::now() - _ticks_start).value());
  LogTarget(Info, gc, phases) log;
  log.print("Balance Page Cache %s Relocation (Sub-phase): %.3fms", _before_relocation ? "Before" : "After", duration);
}

void ZPageCacheBalance::balance() {
  assert(ZBalancePageCache, "sanity");
  assert(Thread::current()->is_ConcurrentGC_thread(), "must balance in a concurrent GC thread");
  if (need_to_balance()) {
    unmap();
    remap();
  }
}

bool ZPageCacheBalance::need_to_balance() {
  // No need to balance page cache if GC is not warm because the heap usage is not used up in this case.
  // The page allocator will create pages from free physical memory instead of flushing page cache.
  // In addition, we can get some early samples of the allocation rate of small and medium objects,
  // which is needed by calculating the target number of pages.
  if (!ZStatCycle::is_warm()) {
    return false;
  }

  ZLocker locker(&_page_allocator->_lock);
  initialize_page_count();
  if (determine_balance_necessity()) {
    calculate_loaner_and_debtor();
    // Loan pages from the page cache into _loaner_list
    _cache->loan_pages(_loaner_count, _loaner_type, _loaner_list);
    return true;
  }
  return false;
}

void ZPageCacheBalance::unmap() {
  // Unmap physical memory
  unmap_pages();
  // Free physical memory
  free_physical_memory();
}

void ZPageCacheBalance::remap() {
  // Create pages from physical memory into _debtor_list
  create_pages_for_debtor();
  // Map physical memory
  map_pages();
  // Insert pages to the page cache
  insert_pages_to_page_cache();
}

void ZPageCacheBalance::initialize_page_count() {
  _available_small = _cache->small_page_count();
  _available_medium = _cache->medium_page_count();
  // the default target page count keep the page cache unchanged
  _target_small = _available_small;
  _target_medium = _available_medium;
}

/*
 * Determine if page cache balance is necessary in this GC cycle, and calculate the target small/medium
 * pages, which should satisfy the lower bound constraint and try to meet the allocation-rate goal:
 * [lower bound constraint] the cached pages are not fewer than the lower bound in order to avoid page cache flush
 *   the lower bound is the smaller value of lower bound 1 & 2:
 *   - lower bound 1: heap capacity * ZMinPageCachePercent / 100  (reserve ZMinPageCachePercent% heap for cached pages)
 *   - lower bound 2: enough to-space for relocation (the cached pages are not fewer than the to-space pages)
 *       Note: lower bound 2 is enabled only before relocation (because it is only designed for relocation)
 *   the lower bound is denoted as "minimal_small" (for small pages) and "minimal_medium" (for medium pages);
 * [allocation-rate goal] the ratio of allocation rate of small/medium pages should match the the ratio
 *    of small/medium pages in the page cache.
 *       Note: allocation-rate goal is enabled only after relocation (because it is designed for allocation)
 *   "optimal_small" and "optimal_medium" means the target small/medium pages which match the ratio and also keep the
 *   page cache capacity unchanged.
 *
 * Example:
 *   Assume that the current page cache contains 8640 small pages and 0 medium pages.
 *   (2MB small page and 32MB medium page, 8640*2MB=17280MB in total)
 *   - The lower bound constraint is 800 small pages (minimal_small) and 50 medium pages (minimal_medium).
 *   - The allocation-rate goal indicates the allocation rate of small/medium pages is 200:1. Therefore, the target pages
 *   are 8000 small pages (optimal_small) and 40 medium pages (optimal_medium), (8000*2MB+40*32MB=17280MB in total)
 *
 *   However, optimal_small and optimal_medium cannot satisfy the lower bound constraint (40 medium pages < 50 medium pages).
 *   By adjusting 40 medium pages to 50 medium pages, the target pages would be 7840 small pages and 50 medium pages.
 *   The target pages keep the page cache capacity unchanged, and they are also the nearest solution to the optimal goal.
 */
bool ZPageCacheBalance::determine_balance_necessity() {
  // [lower bound constraint] The lower bound of small and medium page count
  size_t minimal_medium = calculate_minimal_medium();
  size_t minimal_small = calculate_minimal_small();

  if (calculate_total_size(minimal_small, minimal_medium) > available_page_cache_size()) {
    log_debug(gc, reloc)("Will not balance page cache in this GC cycle "
                         "(the lower bound of page cache size should not be larger than"
                         "current available page cache size)"); // not possible to keep the capacity unchanged
    return false;
  }

  log_debug(gc, reloc)("Allocation Rate: %.3fMB/s (small), %.3fMB/s (medium)",
                       ZStatSmallPageAllocRate::avg() / M, ZStatMediumPageAllocRate::avg() / M);
  // The optimal small and medium page count
  size_t optimal_medium = calculate_optimal_medium();
  size_t optimal_small = calculate_optimal_small();
  guarantee(calculate_total_size(optimal_small, optimal_medium) == available_page_cache_size(),
            "The optimum should not change the size of the page cache.");

  // If the optimal page counts satisfy the lower bound constraint, return them.
  // Otherwise, try to adjust the unsatisfied optimal page count to the lower bound.
  if (optimal_medium >= minimal_medium && optimal_small >= minimal_small) {
    _target_medium = optimal_medium;
    _target_small = optimal_small;
  } else if (optimal_medium < minimal_medium) {
    _target_medium = minimal_medium;
    _target_small = calculate_maximal_small_for_medium(_target_medium);
    guarantee(_target_small >= minimal_small, "small page lower bound");
  } else if (optimal_small < minimal_small) {
    _target_medium = calculate_maximal_medium_for_small(minimal_small);
    // assume integer k, such that calculate_total_size(minimal_small, k) <= available_page_cache_size()
    // minimal_medium is a valid value for k, while _target_medium is the largest value for k
    guarantee(_target_medium >= minimal_medium, "medium page lower bound");
    _target_small = calculate_maximal_small_for_medium(_target_medium);
    // (minimal_small, _target_medium) may not make full use of available page cache,
    // while (_target_small, _target_medium) can make full use
    guarantee(_target_small >= minimal_small, "small page lower bound");
  } else {
    ShouldNotReachHere();
  }

  guarantee(calculate_total_size(_target_small, _target_medium) == available_page_cache_size(),
            "The target should not change the size of the page cache.");
  if (_target_medium == _available_medium) {
    log_debug(gc, reloc)("Will not balance page cache in this GC cycle (no page will be transformed)");
    return false;
  } else {
    log_debug(gc, reloc)("Page Cache (Medium Pages): " SIZE_FORMAT "->" SIZE_FORMAT, _available_medium, _target_medium);
    log_debug(gc, reloc)("Page Cache (Small Pages): " SIZE_FORMAT "->" SIZE_FORMAT, _available_small, _target_small);
    return true;
  }
}

void ZPageCacheBalance::calculate_loaner_and_debtor() {
  if (_target_small > _available_small) {
    _debtor_count = _target_small - _available_small;
    _debtor_type = ZPageTypeSmall;
    _loaner_count = _available_medium - _target_medium;
    _loaner_type = ZPageTypeMedium;
  } else if (_target_medium > _available_medium) {
    _debtor_count = _target_medium - _available_medium;
    _debtor_type = ZPageTypeMedium;
    _loaner_count = _available_small - _target_small;
    _loaner_type = ZPageTypeSmall;
  }
}

void ZPageCacheBalance::unmap_pages() {
  ZListIterator<ZPage> iter(&_loaner_list);
  for (ZPage* page; iter.next(&page); ) {
    _page_allocator->_physical.unmap(page->physical_memory(), page->virtual_memory().start());
  }
}

void ZPageCacheBalance::free_physical_memory() {
  while (!_loaner_list.is_empty()) {
    ZPage* page = _loaner_list.remove_first();
    ZPhysicalMemory &pmem = page->physical_memory();
    ZLocker locker(&_page_allocator->_lock);
    // Free physical memory
    _page_allocator->_physical.free(pmem);
    // Clear physical mapping
    pmem.clear();
    // Add to list of detached pages
    _page_allocator->_detached.insert_last(page);
  }
}

void ZPageCacheBalance::map_pages() {
  ZListIterator<ZPage> iter(&_debtor_list);
  for (ZPage* page; iter.next(&page); ) {
    assert(!page->is_mapped(), "ZPage should not be mapped right after page creation.");
    _page_allocator->map_page(page);
  }
}

void ZPageCacheBalance::create_pages_for_debtor() {
  size_t debtor_page_size = _debtor_type == ZPageTypeSmall ? ZPageSizeSmall : ZPageSizeMedium;
  for (size_t i = 0; i < _debtor_count; i++) {
    ZLocker locker(&_page_allocator->_lock);
    _debtor_list.insert_last(_page_allocator->create_page(_debtor_type, debtor_page_size));
    _page_allocator->increase_used(debtor_page_size, false);
  }
}

void ZPageCacheBalance::insert_pages_to_page_cache() {
  while (!_debtor_list.is_empty()) {
    ZPage* page = _debtor_list.remove_first();
    // Reset page
    page->reset();
    // Update page table
    _pagetable->insert(page);
    // release_page(page, reclaimed) will insert the page to the page cache
    // reclaimed is false because we do not want to update GC statistics for reclaimed bytes
    ZHeap::heap()->release_page(page, false);
  }
}

// Optimal page count: the ratio of allocation rate of small and medium pages should match the the ratio in the page cache
//                    (use the original page count before relocation)
size_t ZPageCacheBalance::calculate_optimal_medium() {
  if (_before_relocation) {
    return _available_medium;
  }
  double medium_rate = ZStatMediumPageAllocRate::avg();
  double small_rate = ZStatSmallPageAllocRate::avg();
  double predicted_medium_ratio = medium_rate / (medium_rate + small_rate + 0.1 /* ensure non-zero */);
  return (size_t) (available_page_cache_size() * predicted_medium_ratio / ZPageSizeMedium);
}

size_t ZPageCacheBalance::calculate_optimal_small() {
  if (_before_relocation) {
    return _available_small;
  }
  // directly calculate it with calculate_optimal_medium()  (the remaining page cache forms small pages)
  return calculate_maximal_small_for_medium(calculate_optimal_medium());
}

// Lower bound constraint: non-zero & ZMinPageCachePercent & enough to-space for relocation (enabled before relocation)
size_t ZPageCacheBalance::calculate_minimal_medium() {
  size_t result = MAX2((size_t)(ZHeap::heap()->capacity() * ZMinPageCachePercent / 100.0 / ZPageSizeMedium), 1UL);
  if (_before_relocation) {
    return MAX2(_medium_selected_to, result);
  }
  return result;
}

size_t ZPageCacheBalance::calculate_minimal_small() {
  size_t result = MAX2((size_t)(ZHeap::heap()->capacity() * ZMinPageCachePercent / 100.0 / ZPageSizeSmall), 1UL);
  if (_before_relocation) {
    result = MAX2(_small_selected_to, result);
  }
  return result;
}

// This function tries to find the maximal small pages to make full use of available page cache size for fixed number of medium pages.
// The result always makes full use of available page cache size.
size_t ZPageCacheBalance::calculate_maximal_small_for_medium(size_t medium) {
  assert(available_page_cache_size() > medium * ZPageSizeMedium, "enough page cache");
  return (available_page_cache_size() - medium * ZPageSizeMedium) / ZPageSizeSmall; // always divisible by ZPageSizeSmall
}

// This function tries to find the maximal medium pages to make full use of available page cache size for fixed number of small pages.
// The result may not always make full use of available page cache size.
size_t ZPageCacheBalance::calculate_maximal_medium_for_small(size_t small) {
  assert(available_page_cache_size() > small * ZPageSizeSmall, "enough page cache");
  return (available_page_cache_size() - small * ZPageSizeSmall) / ZPageSizeMedium;
}

size_t ZPageCacheBalance::available_page_cache_size() {
  return calculate_total_size(_available_small, _available_medium);
}

size_t ZPageCacheBalance::calculate_total_size(size_t small_count, size_t medium_count) {
  return ZPageSizeSmall * small_count + ZPageSizeMedium * medium_count;
}
