#ifndef __PAGE_LOCS_HPP__
#define __PAGE_LOCS_HPP__

#include "global.hpp"

#if EPUB_LINUX_BUILD
  #include <thread>
  #include <mutex>
  #include <fcntl.h>
  #include <mqueue.h>
  #include <sys/stat.h>
#else
  #include "freertos/FreeRTOS.h"
  #include "freertos/task.h"
  #include "freertos/semphr.h"
#endif

#include <map>
#include <set>

class PageLocs
{
  public:
    struct PageId {
      int16_t itemref_index;
      int32_t offset;
      PageId(int16_t idx, int32_t off) {
        itemref_index = idx;
        offset = off;
      }
      PageId() {}
    };

    struct PageInfo {
      int32_t size;
      int16_t page_number;
      PageInfo(int32_t siz, int16_t pg_nbr) {
        size = siz;
        page_number = pg_nbr;
      }
      PageInfo() {};
    };
    typedef std::pair<const PageId, PageInfo> PagePair;

  private:
    static constexpr const char * TAG = "PageLocs";

    bool completed;
    struct PageCompare {
      bool operator() (const PageId & lhs, const PageId & rhs) const { 
        if (lhs.itemref_index < rhs.itemref_index) return true;
        if (lhs.itemref_index > rhs.itemref_index) return false;
        return lhs.offset < rhs.offset; 
      }
    };
    typedef std::map<PageId, PageInfo, PageCompare> PagesMap;
    typedef std::set<int16_t> ItemsSet;

    #if EPUB_LINUX_BUILD
      std::thread state_thread;
      std::thread retriever_thread;
      std::recursive_mutex  mutex;
    #else
      static SemaphoreHandle_t mutex;
      static StaticSemaphore_t mutex_buffer;

      inline static void enter() { xSemaphoreTake(mutex, portMAX_DELAY); }
      inline static void leave() { xSemaphoreGive(mutex); }
    #endif

    PagesMap pages_map;
    ItemsSet items_set;
    int16_t item_count;

    void show();
    bool retrieve_asap(int16_t itemref_index);
    PagesMap::iterator check_and_find(const PageId & page_id);

  public:

    PageLocs() : completed(false), item_count(0) { 
      #if !EPUB_LINUX_BUILD
        mutex = xSemaphoreCreateMutexStatic(&mutex_buffer);
      #endif 
    };

    void setup();
    
    const PageId * get_next_page_id(const PageId & page_id, int16_t count = 1);
    const PageId * get_prev_page_id(const PageId & page_id, int     count = 1);
    const PageId *      get_page_id(const PageId & page_id                   );

    void computation_completed();
    void    start_new_document(int16_t count);

    inline const PageInfo* get_page_info(const PageId& page_id) {
      std::scoped_lock guard(mutex);
      PagesMap::iterator it = check_and_find(page_id);
      return it == pages_map.end() ? nullptr : &it->second;
    }

    inline void insert(PageId & id, PageInfo & info) {
      std::scoped_lock guard(mutex);
      pages_map.insert(std::make_pair(id, info));
      items_set.insert(id.itemref_index);
    }

    inline void clear() { 
      std::scoped_lock guard(mutex);
      pages_map.clear(); 
      items_set.clear();
      completed = false; 
    }

    inline void  size() {
      std::scoped_lock guard(mutex);
      pages_map.size();
    }

    inline int16_t page_count() { return completed ? pages_map.size() : -1; }

    inline int16_t page_nbr(const PageId & id) {
      std::scoped_lock guard(mutex);
      if (!completed) return -1; 
      const PageInfo * info = get_page_info(id);
      return info == nullptr ? -1 : info->page_number;
    };
};

#if __PAGE_LOCS__
  PageLocs page_locs;
#else
  extern PageLocs page_locs;
#endif

#endif