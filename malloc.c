/*
Autor: Wiktor Garbarek
Nr indeksu: 291963

Oświadczam, że jestem jedynym autorem kodu źródłowego.
*/


#include "malloc.h"
#include <sys/queue.h>

#include <sys/mman.h>
#include <unistd.h>
#include <stdint.h>
#include <errno.h>
#include <pthread.h>

typedef struct mem_block mem_block_t;
typedef struct mem_arena mem_arena_t;
typedef LIST_ENTRY(mem_block) mb_node_t;
typedef LIST_ENTRY(mem_arena) ma_node_t;
typedef LIST_HEAD(, mem_block) mb_list_t;
typedef LIST_HEAD(, mem_arena) ma_list_t;

#define MB_ALIGNMENT 2*sizeof(void*)
#define MA_MAXSIZE ((MB_ALIGNMENT*32768)/getpagesize())*getpagesize() //in bytes
#define MA_THRESHOLD MA_MAXSIZE/2

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;


struct mem_block {
  int64_t mb_size; /* mb_size > 0 => free, mb_size < 0 => allocated */
  union {
    mb_node_t mb_link;   /* link on free block list, valid if block is free */
    uint64_t mb_data[0]; /* user data pointer, valid if block is allocated */
  };
};

struct mem_arena {
  void *end_of_arena;
  int64_t padding[2];
  ma_node_t ma_link;     /* link on list of all arenas */
  mb_list_t ma_freeblks; /* list of all free blocks in the arena */
  int64_t size;          /* arena size minus sizeof(mem_arena_t) */
  mem_block_t ma_first;  /* first block in the arena */
};

static ma_list_t *arenas __used = &(ma_list_t){}; /* list of all arenas */

/* ******************************HELPERS******************************/

int64_t abs64(int64_t x){
    return x > 0?x:-x;
}
int64_t sgn(int64_t x){
    return x > 0?1:-1;
}
int64_t max(int64_t x, int64_t y){
    return x > y?x:y;
}

size_t round_size_to_alignment(size_t size, size_t alignment){
    return size + ((size%alignment != 0)?(alignment - (size % alignment)):0);
}

size_t get_size_for_memalign(size_t size){
    return round_size_to_alignment(max(size, 16), MB_ALIGNMENT) + 2*sizeof(int64_t);
}
size_t get_size_for_mmap(size_t size){
    return round_size_to_alignment(max(size, 16), getpagesize());
}

int64_t *get_boundary_tag(char *block_ptr){
    int64_t block_size = abs64(((mem_block_t *) block_ptr)->mb_size);
    return (int64_t *) (block_ptr + block_size - sizeof(int64_t));
}


mem_block_t *get_next_block(char *block_ptr){
    return (mem_block_t *) (block_ptr + abs64(((mem_block_t *)block_ptr)->mb_size));
}
mem_block_t *get_prev_block(char *block_ptr){
    int64_t prev_block_size = *((int64_t *) (block_ptr - sizeof(int64_t)));
    return (mem_block_t *) (((char *) block_ptr) - abs64(prev_block_size));
}

void init_block(mem_block_t *new_block, int64_t size){
    debug("woo i got size %lld", size);
    new_block->mb_size = size;
    set_boundary_tag(new_block);
}

int belongs_to_arena(void *ptr, mem_arena_t *arena){
    return ( (char *) &arena->ma_first <= (char *) ptr) && ((char *) ptr < (char *) arena->end_of_arena);
}

int belongs_to_any_arena(void *ptr){
    mem_arena_t *arena_ptr;
    LIST_FOREACH(arena_ptr, arenas, ma_link) {
        if (belongs_to_arena(ptr, arena_ptr))
            return 1;
    }
    return 0;
}

int is_first_block_on_arena(void *ptr, mem_arena_t *arena){
    return &(arena->ma_first) == ptr;
}
int is_last_block_on_arena(void *ptr, mem_arena_t *arena){
    return !(belongs_to_arena(get_next_block(ptr), arena));
}

void init_arena(mem_arena_t *new_arena, int64_t size){
    new_arena->end_of_arena = (void *) (((char *) new_arena) + size - 24);
    LIST_INSERT_HEAD(arenas, new_arena, ma_link);
    new_arena->size = size;
    LIST_INIT(&new_arena->ma_freeblks);
    init_block(&new_arena->ma_first, size - 16);
    LIST_INSERT_HEAD(&new_arena->ma_freeblks,&new_arena->ma_first, mb_link);
}

void *append_new_arena(size_t size){
    mem_arena_t *new_arena = mmap(NULL, size,
                                  PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANONYMOUS,
                                  -1, 0);

    debug("WE GOT NEW ARENA %p of size %lld", new_arena, size);
    if(new_arena == MAP_FAILED){
        //assert(errno == EINVAL || errno == ENOMEM);
        return MAP_FAILED;
    }

    init_arena(new_arena, size);
    return new_arena;
}

int is_first_block_on_any_arena(void *ptr){
    mem_arena_t *arena_ptr;
    LIST_FOREACH(arena_ptr, arenas, ma_link) {
        if (is_first_block_on_arena(ptr, arena_ptr))
            return 1;
    }
    return 0;
}
int is_last_block_on_any_arena(void *ptr){
    mem_arena_t *arena_ptr;
    LIST_FOREACH(arena_ptr, arenas, ma_link) {
        if (is_last_block_on_arena(ptr, arena_ptr))
            return 1;
    }
    return 0;
}

mem_block_t *get_free_block(size_t size) {
    //assert(size > 0);

    mem_arena_t *arena_ptr;
    LIST_FOREACH(arena_ptr, arenas, ma_link) {
        mem_block_t *block_ptr;
        LIST_FOREACH(block_ptr, &arena_ptr->ma_freeblks, mb_link) {
            debug("arena %p beingfree: %p", block_ptr, arena_ptr);
            if (!belongs_to_arena(block_ptr, arena_ptr)){
                debug("no nie wiem tu jest koniec");
                break;
            }
            if ((int64_t) block_ptr->mb_size >= (int64_t) size) {
                return block_ptr;
            }
        }
    }

    return NULL;
}

mem_arena_t *get_containing_arena(mem_block_t *ptr){
    mem_arena_t *arena_ptr;
    LIST_FOREACH(arena_ptr, arenas, ma_link) {
        if (belongs_to_arena(ptr, arena_ptr))
            return arena_ptr;
    }
}

void set_boundary_tag(mem_block_t *block_ptr){
    int64_t *boundary_tag = get_boundary_tag((char *) block_ptr);
    if(belongs_to_arena(boundary_tag, get_containing_arena(block_ptr))){
        *boundary_tag = abs64(block_ptr->mb_size);
    }
}

void insert_on_free_blocks_list(mem_block_t *block_ptr, mem_arena_t *arena){
    mem_block_t *cur_block_ptr;
    LIST_FOREACH(cur_block_ptr, &arena->ma_freeblks, mb_link) {
      if (cur_block_ptr > block_ptr) {
          LIST_INSERT_BEFORE(cur_block_ptr, block_ptr, mb_link);
          break;
      }
      if (LIST_NEXT(cur_block_ptr, mb_link) == NULL){
          LIST_INSERT_AFTER(cur_block_ptr, block_ptr, mb_link);
          break;
      }
    }
}

int is_free(mem_block_t *block_ptr){
    return block_ptr->mb_size > 0;
}

mem_block_t *get_mem_block(void *user_ptr){
    return (mem_block_t *) (((char *) user_ptr) - sizeof(int64_t));
}

void memory_dump(){
    mem_arena_t *arena_ptr;
    LIST_FOREACH(arena_ptr, arenas, ma_link) {
        debug("Arena %p of size %zu", arena_ptr, arena_ptr->size);
        debug("first_block %p of size %zu", &arena_ptr->ma_first, arena_ptr->ma_first.mb_size);
        char *block_ptr = (char *) (&arena_ptr->ma_first);
        for (;belongs_to_arena(block_ptr, arena_ptr); block_ptr += abs64(((mem_block_t *) block_ptr)->mb_size)) {
            debug("    block %p of size %zu", block_ptr, ((mem_block_t * ) block_ptr)->mb_size);
            debug("    it has boundary_tag %d", (int64_t) abs64(*get_boundary_tag(block_ptr)));
            debug("    is it first? %d",is_first_block_on_arena(block_ptr, arena_ptr));
            debug("    is it last?  %d",is_last_block_on_arena(block_ptr, arena_ptr));
            debug("    is it free?  %d", is_free(block_ptr));
            debug("    *~*~*~*~*~*~*~");
        }
        debug("~*~*~*~*~*~*~*~*~*~*~*~");
    }
}

/* ***********************UNSAFE MALLOC*****************************/


void __my_unsafe_free(void *ptr) {
//    return;
  debug("%s(%p)", __func__, ptr);
  return;
  if(ptr == NULL){
      return;
  }

  mem_block_t *block_ptr = get_mem_block(ptr);


  if (!belongs_to_any_arena(block_ptr)){
      return;
  }
  if (block_ptr->mb_size >= 0){
      return;
  }
  block_ptr->mb_size *= -1; //now free
  mem_block_t *left_guy, *right_guy;
  if(!is_first_block_on_any_arena(block_ptr)){
      left_guy = get_prev_block((char *) block_ptr);
      if (!is_free(left_guy)){
          left_guy = NULL;
      }
  }
  if(!is_last_block_on_any_arena(block_ptr)){
      right_guy = get_next_block((char *) block_ptr);
      if (!is_free(right_guy)){
          right_guy = NULL;
      }
  }
  mem_arena_t *proper_arena = get_containing_arena(block_ptr);

  mem_block_t *right_block = right_guy;
  mem_block_t *left_block = left_guy;

  int merge_count = 0;
  if(left_guy != NULL){
      //memory_dump();
      merge_count++;
      init_block(left_block, abs64(left_block->mb_size) + abs64(block_ptr->mb_size));
      block_ptr = left_block;
  }
  if(right_guy != NULL){
      merge_count++;
      init_block(block_ptr, abs64(block_ptr->mb_size) + abs64(right_block->mb_size));
      LIST_INSERT_BEFORE(right_block, block_ptr, mb_link);
      LIST_REMOVE(right_block, mb_link);
  }

 // mem_arena_t *proper_arena = get_containing_arena(block_ptr);

  if (merge_count == 0){
      //if we didnt merge with any block nearby then
      //this free block has to be inserted on the list of free blks
      insert_on_free_blocks_list(block_ptr, proper_arena);
  }

  if (is_first_block_on_arena(block_ptr, proper_arena) && is_last_block_on_arena(block_ptr, proper_arena)){
      munmap(proper_arena, proper_arena->size);
  }


}

void *__my_unsafe_memalign(size_t alignment, size_t size) {
  debug("%s(%ld, %ld)", __func__, alignment, size);
    if ( !powerof2(alignment) || (alignment % sizeof(void*) != 0)){
      debug("alignmentproblem");
      errno = EINVAL;
      return NULL;
    }
    debug("do we make new huge chunk? %d > %d --> %d", size, MA_MAXSIZE, size > MA_MAXSIZE);
    if (size > MA_MAXSIZE){
        if (size >= SIZE_MAX - sizeof(mem_arena_t)){ //results in overflow so we cant allow it
            debug("size max");
            errno = ENOMEM;
            return NULL;
        }
        size_t rounded_size = round_size_to_alignment(size + sizeof(mem_arena_t), getpagesize());
        debug("big_rounded_size %lld", rounded_size);
        void *new_arena = append_new_arena(rounded_size);

        if(new_arena == MAP_FAILED){
          debug("map");
          errno = ENOMEM;
          return NULL;
        }
        ((mem_arena_t *) new_arena)->ma_first.mb_size *= -1;
        LIST_REMOVE(&(((mem_arena_t *) new_arena)->ma_first), mb_link);
        return &((mem_arena_t *) new_arena)->ma_first.mb_data[0];
    }

    size_t memalign_size = get_size_for_memalign(size + alignment);
//    memory_dump();
    mem_block_t *ptr = get_free_block(memalign_size);
    if (ptr == NULL){
        append_new_arena(MA_MAXSIZE);
        ptr = get_free_block(memalign_size);
    }
    if (ptr == NULL){
        debug("getfreeblockfailed");
        errno = ENOMEM;
        return NULL;
    }
    size_t current_size = ptr->mb_size;

    mem_block_t *aligned_ptr_metadata = ((char *) ptr) + (alignment - ((uintptr_t) ptr % alignment)) - sizeof(void *);
    size_t left_block = (uintptr_t) aligned_ptr_metadata - (uintptr_t) ptr;
    debug("first - memalign_size %d", memalign_size);
    debug("current_size %d, left_block %d", current_size, left_block);
    if (left_block != 0){
        if (left_block == 16){ //left block is too small, try to merge it with neighbour
            LIST_INSERT_AFTER(ptr, aligned_ptr_metadata, mb_link);
            LIST_REMOVE(ptr, mb_link);
            init_block(aligned_ptr_metadata, current_size - left_block);

            mem_block_t *prev_block = get_prev_block(ptr);
            init_block(prev_block, abs64(prev_block->mb_size) + left_block);
            if(!is_free(prev_block)){
                prev_block->mb_size *= -1;
            }
        } else{
            init_block(ptr, left_block);
            init_block(aligned_ptr_metadata, current_size - left_block);
            LIST_INSERT_AFTER(ptr, aligned_ptr_metadata, mb_link);
        }
    }
//    memory_dump();
    size_t final_size = get_size_for_memalign(size);
    mem_arena_t *proper_arena = get_containing_arena(aligned_ptr_metadata);
    if (aligned_ptr_metadata->mb_size - final_size >= 32){
        mem_block_t *right_block = ((char *) aligned_ptr_metadata) + final_size;
        if (belongs_to_arena(right_block, proper_arena)){
            init_block(right_block, aligned_ptr_metadata->mb_size - final_size);
            LIST_INSERT_AFTER(aligned_ptr_metadata, right_block, mb_link);
            init_block(aligned_ptr_metadata, final_size);
        }
    }
    LIST_REMOVE(aligned_ptr_metadata, mb_link);
    aligned_ptr_metadata->mb_size *= -1;
//    memory_dump();
    return &aligned_ptr_metadata->mb_data[0];
}

void *__my_unsafe_malloc(size_t size) {
  debug("%s(%ld)", __func__, size);
  void *ptr = __my_unsafe_memalign(MB_ALIGNMENT, size);
  if (ptr == NULL){
      //errno is already set, isnt it?
      return NULL;
  }
  return ptr;
}
void *__my_primitive_realloc(void *ptr, size_t size){
    if (ptr == NULL){
        return __my_unsafe_malloc(size);
    }
    if (size == 0){
        __my_unsafe_free(ptr);
        return NULL;
    }
    if (size > MA_MAXSIZE){
        if (size >= SIZE_MAX - sizeof(mem_arena_t)){ //results in overflow so we cant allow it
            debug("size max");
            errno = ENOMEM;
            return NULL;
        }
    }
    mem_block_t *block_ptr = get_mem_block(ptr);
    void* new_data_ptr = __my_unsafe_malloc(size);
    memcpy(new_data_ptr, ptr, abs64(block_ptr->mb_size));
    __my_unsafe_free(ptr);
    return new_data_ptr;
}

void *__my_unsafe_realloc(void *ptr, size_t size) {
//    return NULL;
  debug("%s(%p, %ld)", __func__, ptr, size);
  if (ptr == NULL){
      return __my_unsafe_malloc(size);
  }
  if (size == 0){
      __my_unsafe_free(ptr);
      return NULL;
  }
  mem_block_t *block_ptr = get_mem_block(ptr);
  int64_t block_size = abs64(block_ptr->mb_size);

  if (size > MA_MAXSIZE){
      if (size >= SIZE_MAX - sizeof(mem_arena_t)){ //results in overflow so we cant allow it
          debug("size max");
          errno = ENOMEM;
          return NULL;
      }
      size_t rounded_size = round_size_to_alignment(size + sizeof(mem_arena_t), getpagesize());
      void *new_arena = append_new_arena(rounded_size);
      memcpy(&((mem_arena_t *) new_arena)->ma_first.mb_data[0], ptr, block_size);

      if(new_arena == MAP_FAILED){
        debug("map");
        errno = ENOMEM;
        return NULL;
      }

      return &((mem_arena_t *) new_arena)->ma_first.mb_data[0];
  }


  int64_t rounded_size = get_size_for_memalign(size);//round_size_to_alignment(size + 2*sizeof(void *), MB_ALIGNMENT);
  if (block_size > rounded_size){
    //we truncate this block
    //[M|X|X|X|X|X|X|X|X|X|X|B|M| | | | | ]
    //        becomes
    //[M|X|X|X|X|X|X|B|M|x|x|b|m| | | | | ]
    //  ^ptr                  ^next_block
    //^block_ptr      ^middle_block
    init_block(block_ptr, rounded_size);
    void *middle_block = get_next_block(block_ptr);
    mem_arena_t *proper_arena = get_containing_arena(middle_block);
    insert_on_free_blocks_list(middle_block, proper_arena);
    return ptr;
  }
  if (block_size < rounded_size){
      mem_block_t *next_block = get_next_block(block_ptr);
      if (next_block->mb_size >= rounded_size - block_size){ //automaticaly it has to be free since > 0
          size_t current_next_size = next_block->mb_size;
          init_block(next_block, rounded_size - block_size);
          mem_block_t *remaining_block = get_next_block(next_block);
          init_block(remaining_block, current_next_size - rounded_size + block_size);

          mem_arena_t *proper_arena = get_containing_arena(next_block);
          insert_on_free_blocks_list(remaining_block, proper_arena);
      } else{
          void* new_data_ptr = __my_unsafe_malloc(rounded_size);
          memcpy(new_data_ptr, ptr, block_size);
          __my_unsafe_free(ptr);
          return new_data_ptr;
      }

  }
  return ptr;

}

size_t __my_unsafe_malloc_usable_size(void *ptr) {
  debug("%s(%p)", __func__, ptr);
  return 0;
}


/* This procedure is called before any allocation happens. */
__constructor void __malloc_init(void) {
  __malloc_debug_init();
  LIST_INIT(arenas);
  append_new_arena(MA_MAXSIZE);
  memory_dump();
  __my_unsafe_memalign(8, 0);
  memory_dump();
  __my_unsafe_memalign(256, 10);
  memory_dump();
}

void __my_free(void *ptr){
    pthread_mutex_lock(&mutex);
    __my_unsafe_free(ptr);
    pthread_mutex_unlock(&mutex);
}

void *__my_malloc(size_t size){
    pthread_mutex_lock(&mutex);
    void *result = __my_unsafe_malloc(size);
    pthread_mutex_unlock(&mutex);
    return result;
}

void *__my_realloc(void *ptr, size_t size){
    pthread_mutex_lock(&mutex);
    void *result = __my_primitive_realloc(ptr, size);
    pthread_mutex_unlock(&mutex);
    return result;
}

void *__my_memalign(size_t alignment, size_t size){
    pthread_mutex_lock(&mutex);
    void *result = __my_unsafe_memalign(alignment, size);
    pthread_mutex_unlock(&mutex);
    return result;
}

size_t __my_malloc_usable_size(void *ptr){
    pthread_mutex_lock(&mutex);
    size_t result = __my_unsafe_malloc_usable_size(ptr);
    pthread_mutex_unlock(&mutex);
    return result;
}

/* DO NOT remove following lines */
__strong_alias(__my_free, cfree);
__strong_alias(__my_free, free);
__strong_alias(__my_malloc, malloc);
__strong_alias(__my_malloc_usable_size, malloc_usable_size);
__strong_alias(__my_memalign, aligned_alloc);
__strong_alias(__my_memalign, memalign);
__strong_alias(__my_realloc, realloc);
