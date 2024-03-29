#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/mman.h>

#include "wasm_export.h"
#include "wasmdefs.h"

#include <map>
#include <set>
#include <unordered_set>
#include <mutex>
#include <vector>
#include <fstream>
#include <atomic>

#define INSTRUMENT 1
#define TRACE_ACCESS 0

/* Timing */
uint64_t start_ts;
uint64_t end_ts;

static inline uint64_t ts2us(struct timespec ts) {
  return (ts.tv_sec * 1000000) + (ts.tv_nsec / 1000);
}

uint64_t gettime() {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return ts2us(ts);
}
/* */

typedef std::unordered_set<uint32_t> InstSet;

/* Access Logger */
struct acc_entry {
  uint64_t last_tid;
  InstSet inst_idxs;
  uint64_t freq;
  bool shared;
  bool write_encountered;
  std::mutex mtx;
};

std::mutex mtx;
acc_entry *access_table = NULL;
size_t table_size = sizeof(acc_entry) * ((size_t)1 << 32);
uint32_t addr_min = -1;
uint32_t addr_max = 0;

std::mutex shared_inst_mtx;
std::set<uint32_t> shared_inst_idxs;
/*  */

void logaccess_wrapper(wasm_exec_env_t exec_env, uint32_t addr, uint32_t opcode, uint32_t inst_idx) {
  #if INSTRUMENT == 1
  uint64_t tid = wasm_runtime_get_exec_env_uid(exec_env);
  bool is_write = (opcode_access[opcode].type == STORE);
  #if TRACE_ACCESS == 1
  printf("I: %u | A: %u | T: %lu\n", inst_idx, addr, tid);
  #endif
  acc_entry *entry = access_table + addr;

  entry->mtx.lock();
  bool new_tid_acc = (tid != entry->last_tid);
  /* First access to address: Construct instruction set */
  if (!entry->last_tid) {
    new (&entry->inst_idxs) InstSet;
    entry->inst_idxs.insert(inst_idx);
  }
  /* Shared accesses from any thread write to global set */
  else if (entry->shared) {
    shared_inst_mtx.lock();
    shared_inst_idxs.insert(inst_idx);
    shared_inst_mtx.unlock();
  }
  /* Unshared access from new thread: Mark as shared and append logged insts */
  else if (new_tid_acc) {
    entry->shared = true;
    shared_inst_mtx.lock();
    shared_inst_idxs.insert(entry->inst_idxs.begin(), entry->inst_idxs.end());
    shared_inst_idxs.insert(inst_idx);
    shared_inst_mtx.unlock();
    /* Save some memory by deleting unused set */
    entry->inst_idxs.~InstSet();
  }
  /* Unshared access from only one thread: Log inst */
  else {
    entry->inst_idxs.insert(inst_idx);
  }
  entry->last_tid = tid;
  entry->freq += 1;
  entry->write_encountered = is_write;
  entry->mtx.unlock();

  addr_min = (addr < addr_min) ? addr : addr_min;
  addr_max = (addr > addr_max) ? addr : addr_max;
  #endif
}

#define FWRITE(elem) outfile.write((char*) &elem, sizeof(elem));

#define BWRITE(elem) {  \
  char* addr = (char*) &elem; \
  partials.insert(partials.end(), addr, addr + sizeof(elem));  \
}

#define BWRITE_FIX(ptr, sz) { \
  char* addr = (char*) ptr; \
  partials.insert(partials.end(), addr, addr + sz); \
}

typedef uint8_t byte;

void logend_wrapper(wasm_exec_env_t exec_env) {
  end_ts = gettime();
  float total_time = (float)(end_ts - start_ts) / 1000000; 
  printf("========= LOGEND ===========\n");
  fprintf(stderr, "Time: %.3f\n", total_time);

  #if INSTRUMENT == 1
  char logfile[] = "shared_mem.bin";
  std::ofstream outfile(logfile, std::ios::out | std::ios::binary);

  std::vector<uint32_t> inst_idxs(shared_inst_idxs.begin(), shared_inst_idxs.end());
  /* Dump print */
  for (auto &i : inst_idxs) {
    printf("%u ", i);
  }
  printf("\n");

  /* Read memory size from wamr API */
  uint32_t mem_size = wasm_runtime_get_memory_size(get_module_inst(exec_env));
  if (addr_max > mem_size) {
    printf("ERROR in mem size (%u) calculation (less than max addr: %u)\n", mem_size, addr_max);
  }
  printf("Mem size: %u\n", mem_size);
  printf("Addr min: %u | Addr max: %u\n", addr_min, addr_max);

  /* Log shared instructions */
  uint32_t num_inst_idxs = inst_idxs.size();
  FWRITE(num_inst_idxs);
  outfile.write((char*) inst_idxs.data(), num_inst_idxs * sizeof(uint32_t));

  std::vector<uint32_t> shared_addrs;
  std::vector<byte> partials;

  /* Access table dump  */
  printf("=== ACCESS TABLE ===\n");
  for (uint32_t i = 0; i < mem_size; i++) {
    acc_entry *entry = access_table + i;
    if (entry->last_tid) {
      if (entry->shared) {
        printf("Addr [%u] | Accesses: %lu [SHARED]\n", i, entry->freq);
        shared_addrs.push_back(i);
      } else {
        printf("Addr [%u] | Accesses: %lu\n", i, entry->freq);
        /* Write partial content */
        BWRITE (i);
        BWRITE (entry->last_tid);
        BWRITE (entry->write_encountered);
        std::vector<uint32_t> entry_idxs(entry->inst_idxs.begin(), entry->inst_idxs.end());
        uint32_t num_entry_idxs = entry_idxs.size();
        BWRITE (num_entry_idxs);
        BWRITE_FIX (entry_idxs.data(), num_entry_idxs * sizeof(uint32_t));
      }
    }
  }

  /* Log shared addrs */
  uint32_t num_shared_addrs = shared_addrs.size();
  FWRITE(num_shared_addrs);
  outfile.write((char*) shared_addrs.data(), num_shared_addrs * sizeof(uint32_t));

  /* Log partial addr + idx */
  outfile.write((char*) partials.data(), partials.size());

  printf("Written data to %s\n", logfile);
  #endif
  int status = munmap(access_table, table_size);
  if (status == -1) {
    perror("munmap error");
  }
}


/* Initialization routine */
void init_acc_table() {
  access_table = (acc_entry*) mmap(NULL, table_size, PROT_READ|PROT_WRITE, 
                    MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE, -1, 0);
  if (access_table == NULL) {
    perror("malloc error");
  }
}

void logstart_wrapper(wasm_exec_env_t exec_env, uint32_t max_instructions) {
  static std::atomic_bool first {false};
  static bool first_done = false;
  /* Init functions should only happen once */
  if (first.exchange(true) == false) {
    init_acc_table();
    start_ts = gettime();
    first_done = true;
  }
  else {
    while (!first_done) { };
  }
}



/* WAMR Registration Hooks */
#define REG_NATIVE_FUNC(func_name, sig) \
  { #func_name, (void*) func_name##_wrapper, sig, NULL }
static NativeSymbol native_symbols[] = {
  REG_NATIVE_FUNC(logstart, "(i)"),
  REG_NATIVE_FUNC(logaccess, "(iii)"),
  REG_NATIVE_FUNC(logend, "()")
};


extern "C" uint32_t get_native_lib (char **p_module_name, NativeSymbol **p_native_symbols) {
  *p_module_name = "instrument";
  init_acc_table();
  *p_native_symbols = native_symbols;
  return sizeof(native_symbols) / sizeof(NativeSymbol);
}

