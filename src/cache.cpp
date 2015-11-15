#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "cache.h"
//#define WJP_DEBUG
extern uns64 SWP_CORE0_WAYS;
extern uns64 cycle; // You can use this as timestamp for LRU

////////////////////////////////////////////////////////////////////
// ------------- DO NOT MODIFY THE INIT FUNCTION -----------
////////////////////////////////////////////////////////////////////

Cache  *cache_new(uns64 size, uns64 assoc, uns64 linesize, uns64 repl_policy){

   Cache *c = (Cache *) calloc (1, sizeof (Cache));
   c->num_ways = assoc;
   c->repl_policy = repl_policy;

   if(c->num_ways > MAX_WAYS){
     printf("Change MAX_WAYS in cache.h to support %llu ways\n", c->num_ways);
     exit(-1);
   }

   // determine num sets, and init the cache
   c->num_sets = size/(linesize*assoc);
   c->sets  = (Cache_Set *) calloc (c->num_sets, sizeof(Cache_Set));

   return c;
}

////////////////////////////////////////////////////////////////////
// ------------- DO NOT MODIFY THE PRINT STATS FUNCTION -----------
////////////////////////////////////////////////////////////////////

void    cache_print_stats    (Cache *c, char *header){
  double read_mr =0;
  double write_mr =0;

  if(c->stat_read_access){
    read_mr=(double)(c->stat_read_miss)/(double)(c->stat_read_access);
  }

  if(c->stat_write_access){
    write_mr=(double)(c->stat_write_miss)/(double)(c->stat_write_access);
  }

  printf("\n%s_READ_ACCESS    \t\t : %10llu", header, c->stat_read_access);
  printf("\n%s_WRITE_ACCESS   \t\t : %10llu", header, c->stat_write_access);
  printf("\n%s_READ_MISS      \t\t : %10llu", header, c->stat_read_miss);
  printf("\n%s_WRITE_MISS     \t\t : %10llu", header, c->stat_write_miss);
  printf("\n%s_READ_MISS_PERC  \t\t : %10.3f", header, 100*read_mr);
  printf("\n%s_WRITE_MISS_PERC \t\t : %10.3f", header, 100*write_mr);
  printf("\n%s_DIRTY_EVICTS   \t\t : %10llu", header, c->stat_dirty_evicts);

  printf("\n");
}



////////////////////////////////////////////////////////////////////
// Note: the system provides the cache with the line address
// Return HIT if access hits in the cache, MISS otherwise 
// Also if is_write is TRUE, then mark the resident line as dirty
// Update appropriate stats
////////////////////////////////////////////////////////////////////
//address <Tag><Index>58-6
//numsets 64 in dcache, so index bit is 6
static inline int     index_bit(Cache* c){
  uns64 tmp=1, num_sets=c->num_sets;
  for(int i=0;i<64;i++){
    if(tmp==num_sets)return i;
    tmp<<=1;
  }
  return -1;
}
static inline Addr     tag_of_addr(Cache* c, Addr addr){
  addr>>=(index_bit(c));  // unsigned >> will assign 0
  return addr;
}  
static inline uns      index_of_addr(Cache*c, Addr addr){
  Addr tmp=1;
  for(int i=0,t=index_bit(c)-1;i<t;i++){
    tmp|=(tmp<<1);
  }
  addr&=tmp; 
  return (uns)addr;
}
static inline bool     hit_line(Cache_Line* line, Addr tag, uns core_id){
  if(line->valid&&line->tag==tag&&line->core_id==core_id){
    return true;
  }
  return false;
}
static inline void     update_access_stat(Cache*c, uns is_write){
  if(is_write){
    c->stat_write_access++;
  }
  else{
    c->stat_read_access++;
  }
}
static inline void     update_miss_stat(Cache*c,uns is_write){
  if(is_write){
    c->stat_write_miss++;
  }
  else{
    c->stat_read_miss++;
  }
}  
Flag cache_access(Cache *c, Addr lineaddr, uns is_write, uns core_id){
  Flag outcome=MISS;
  update_access_stat(c,is_write);
#ifdef WJP_DEBUG
  static int i=0;
  if(i<=10){
    i++;
    printf("%016llX: tag=%016llX, index=%d\n", lineaddr, tag_of_addr(c,lineaddr),index_of_addr(c,lineaddr));
  }
#endif
  Addr       tag   = tag_of_addr(c,lineaddr);
  uns        index = index_of_addr(c, lineaddr);
  Cache_Set* set   = &c->sets[index];
  for(uns64 i=0;i<c->num_ways;i++){
    Cache_Line* line=&set->line[i];
    if(hit_line(line,tag,core_id)){   
      outcome=HIT;
      if(is_write){
	line->dirty=TRUE;
      }
      line->last_access_time=cycle;
      break;
    }
  }
  if(outcome==MISS){
    update_miss_stat(c,is_write);
  }
  return outcome;
}

////////////////////////////////////////////////////////////////////
// Note: the system provides the cache with the line address
// Install the line: determine victim using repl policy (LRU/RAND)
// copy victim into last_evicted_line for tracking writebacks
////////////////////////////////////////////////////////////////////
void cache_install(Cache *c, Addr lineaddr, uns is_write, uns core_id){
  uns index=index_of_addr(c,lineaddr);
  uns way_id=cache_find_victim(c,index,core_id);
  Cache_Line* line=&c->sets[index].line[way_id];
  if(line->valid){
    c->last_evicted_line=*line;
    if(line->dirty){
      c->stat_dirty_evicts++;
    }  
  }
  line->valid=TRUE;
  line->dirty=FALSE;
  if(is_write){
    line->dirty=TRUE;
  }
  line->tag=tag_of_addr(c,lineaddr);         //actually there is no need to use tag, <tag><index> also works 
  line->core_id=core_id;
  line->last_access_time=cycle;
}

////////////////////////////////////////////////////////////////////
// You may find it useful to split victim selection from install
////////////////////////////////////////////////////////////////////
static inline uns      lru_victim(Cache *c, uns set_index, uns core_id){
  Cache_Set*set=&c->sets[set_index];
  uns oldest=set->line[0].last_access_time;
  uns oldest_index=0;
  for(uns64 i=1;i<c->num_ways;i++){
    if(set->line[i].last_access_time < oldest){
      oldest_index=i;
      oldest=set->line[i].last_access_time;
    }  
  }  
  return oldest_index;
}  
uns cache_find_victim(Cache *c, uns set_index, uns core_id){
  Cache_Set*set=&c->sets[set_index];
  for(uns64 i=0;i<c->num_ways;i++){
    if(!set->line[i].valid){
      return (uns)i;
    }  
  }  
  switch(c->repl_policy){
  case 0://lru
    return lru_victim(c,set_index,core_id);
  case 1://rand
    return -1;
  case 2://swp part e
    return -1;
  case 3://part f
    return -1;
  default:
    return -1;
  }
}
