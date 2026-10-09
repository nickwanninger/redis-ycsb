// Built-in YCSB workload runner.
// Keep API pointers local to avoid symbols from other built-in modules.
#define REDISMODULE_API static
extern "C" {
#include "../src/redismodule.h"
}

#include "core/client.h"
#include "core/core_workload.h"
#include "core/db.h"
#include "core/timer.h"
#include "core/utils.h"

#include <algorithm>
#include <chrono>
#include <dlfcn.h>
#include <iostream>
#include <random>
#include <string>
#include <sys/resource.h>
#include <vector>

long get_rss_kb() {
  struct rusage usage;
  if (getrusage(RUSAGE_SELF, &usage) != 0)
    return -1;

#ifdef __APPLE__
  return usage.ru_maxrss / 1024; // macOS returns bytes
#else
  return usage.ru_maxrss; // Linux returns kilobytes
#endif
}

size_t get_rss_bytes(void) {
  long rss_pages = 0;
  FILE *f = fopen("/proc/self/statm", "r");
  if (f) {
    if (fscanf(f, "%*s%ld", &rss_pages) != 1)
      rss_pages = 0;
    fclose(f);
  }
  return (size_t)rss_pages * (size_t)0x1000;
}

using std::cout;
using std::endl;

static inline uint64_t read_cycle_counter() {
#if defined(__x86_64__) || defined(__i386__)
  uint32_t lo, hi;
  __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
  return ((uint64_t)hi << 32) | lo;
#elif defined(__riscv)
  uint64_t cycles;
  __asm__ volatile("rdcycle %0" : "=r"(cycles));
  return cycles;
#else
#error "Unsupported architecture"
#endif
}

// Forward decl
void PrintReply(RedisModuleCallReply *rep, int indent);

void PrintIndent(int indent) {
  for (int i = 0; i < indent; i++)
    printf("  ");
}

void PrintReply(RedisModuleCallReply *rep, int indent) {
  auto t = RedisModule_CallReplyType(rep);

  switch (t) {
  case REDISMODULE_REPLY_STRING:
  case REDISMODULE_REPLY_ERROR: {
    size_t len;
    const char *s = RedisModule_CallReplyStringPtr(rep, &len);
    PrintIndent(indent);
    printf("\"%.*s\"\n", (int)len, s);
    break;
  }

  case REDISMODULE_REPLY_INTEGER: {
    long long v = RedisModule_CallReplyInteger(rep);
    PrintIndent(indent);
    printf("%lld\n", v);
    break;
  }

  case REDISMODULE_REPLY_NULL:
    PrintIndent(indent);
    printf("NULL\n");
    break;

  case REDISMODULE_REPLY_ARRAY: {
    size_t n = RedisModule_CallReplyLength(rep);
    PrintIndent(indent);
    printf("[array of %zu]\n", n);

    for (size_t i = 0; i < n; i++) {
      RedisModuleCallReply *elem = RedisModule_CallReplyArrayElement(rep, i);
      PrintReply(elem, indent + 1);
    }
    break;
  }

  default:
    PrintIndent(indent);
    printf("<unknown reply type>\n");
    break;
  }
}

static void toggle_printing(int enable) {
  typedef void (*yukon_enable_printing_t)(bool enable);
  static yukon_enable_printing_t yukon_enable_printing =
      (yukon_enable_printing_t)dlsym(RTLD_DEFAULT, "yukon_enable_printing");
  if (yukon_enable_printing != NULL) {
    yukon_enable_printing(enable);
  }
}

class RedisCommandBuilder {
private:
  std::vector<std::string> args;

public:
  RedisCommandBuilder &command(const std::string &cmd) {
    args.clear();
    args.push_back(cmd);
    return *this;
  }

  RedisCommandBuilder &arg(const std::string &argument) {
    args.push_back(argument);
    return *this;
  }

  int execute(RedisModuleCtx *ctx) {
    RedisModuleCallReply *reply = nullptr;

    if (args.size() == 2) {
      toggle_printing(1);
      reply = RedisModule_Call(ctx, args[0].data(), "c", args[1].data());
      toggle_printing(0);
    } else {
      std::vector<RedisModuleString *> argv;
      for (size_t i = 1; i < args.size(); ++i) {
        argv.push_back(
            RedisModule_CreateString(ctx, args[i].data(), args[i].size()));
      }

      toggle_printing(1);
      reply =
          RedisModule_Call(ctx, args[0].data(), "v", argv.data(), argv.size());
      toggle_printing(0);

      for (size_t i = 0; i < argv.size(); ++i) {
        RedisModule_FreeString(ctx, argv[i]);
      }
    }

    if (reply == nullptr) {
      printf("Error executing command:");
      for (const auto &arg : args) {
        printf(" %s", arg.c_str());
      }
      printf("\n");
    } else if (RedisModule_CallReplyType(reply) == REDISMODULE_REPLY_ERROR) {
      printf("Error in command reply: %s\n",
             RedisModule_CallReplyStringPtr(reply, nullptr));
      RedisModule_FreeCallReply(reply);
      return -1;
    } else {
      // PrintReply(reply, 0);
    }

    // Handle the reply if needed
    if (reply != nullptr) {
      RedisModule_FreeCallReply(reply);
    }

    return 0;
  }
};

namespace ycsbc {

class RedisInternalDB : public DB {
public:
  RedisModuleCtx *ctx;
  RedisInternalDB(RedisModuleCtx *ctx) : ctx(ctx) {}

  void Init() {}

  int Read(const std::string &table, const std::string &key,
           const std::vector<std::string> *fields,
           std::vector<KVPair> &result) {
    // Note: i intentionally ignore result.
    RedisCommandBuilder cmd;
    if (fields) {

      // HMGT KEY FIELD1 FIELD2 ...
      cmd.command("HMGET").arg(key);

      for (const auto &field : *fields) {
        cmd.arg(field);
      }
    } else {
      cmd.command("HGETALL").arg(key);
    }

    return cmd.execute(ctx);
  }

  int Scan(const std::string &table, const std::string &key, int len,
           const std::vector<std::string> *fields,
           std::vector<std::vector<KVPair>> &result) {
    throw "Scan: function not implemented!";
    return 0;
  }

  int Update(const std::string &table, const std::string &key,
             std::vector<KVPair> &values) {
    RedisCommandBuilder cmd;
    cmd.command("HMSET").arg(key);

    // printf("HMSET %s\n", key.data());

    for (auto &p : values) {
      auto &key = p.first;
      auto &value = p.second;
      if (value.length() == 0) {
        value = "X";
      }
      // printf("  '%s' = '%s'\n", key.data(), value.data());
      cmd.arg(key).arg(value);
    }

    return cmd.execute(ctx);
  }

  int Insert(const std::string &table, const std::string &key,
             std::vector<KVPair> &values) {
    return Update(table, key, values);
  }

  int Delete(const std::string &table, const std::string &key) {
    RedisCommandBuilder cmd;
    cmd.command("DEL").arg(key);
    return cmd.execute(ctx);
  }

private:
  std::mutex mutex_;
};

} // namespace ycsbc

static inline uint64_t read_instret() {
  uint64_t instret;
#ifdef __riscv
  asm volatile("rdinstret %0" : "=r"(instret));
#else
  instret = 0;
#endif
  return instret;
}

static void change_roi(int enabled) {
  typedef void (*yukon_switch_roi_t)(int enabled);
  static yukon_switch_roi_t yukon_change_roi =
      (yukon_switch_roi_t)dlsym(RTLD_DEFAULT, "yukon_change_roi");
  if (yukon_change_roi != NULL) {
    yukon_change_roi(enabled);
  }
}

// Option 2: Reservoir sampling for exact percentiles
// Best for: Unknown range, exact percentiles needed
typedef void (*yukon_enable_localization_t)(bool enable);

int DelegateClient(ycsbc::DB *db, ycsbc::CoreWorkload *wl, const int num_ops,
                   bool is_loading) {
  yukon_enable_localization_t yukon_enable_localization =
      (yukon_enable_localization_t)dlsym(RTLD_DEFAULT,
                                         "yukon_enable_localization");

  db->Init();
  ycsbc::Client client(*db, *wl);
  int oks = 0;
  int bads = 0;
  if (yukon_enable_localization != NULL) {
    yukon_enable_localization(!is_loading);
  }
  if (is_loading) {

    // read all the lines from /proc/self/maps and print them. use the FILE c
    // api
    FILE *maps_file = fopen("/proc/self/maps", "r");
    char line[512];
    if (maps_file) {
      while (fgets(line, sizeof(line), maps_file)) {
        printf("YUKON_MAP_REGION=%s", line);
      }
      fclose(maps_file);
    }

    printf("Loading %d keys...\n", num_ops);
  } else {
    printf("Performing %d ops...\n", num_ops);
    change_roi(1);
    // printf("YUKON_BEGIN_CRITICAL=%zu\n", read_cycle_counter()); //
  }

  int reporting_interval = std::max(1, num_ops / 100);
  int next_report = reporting_interval;
  auto start_inst = read_instret();

  auto report_start_inst = start_inst;
  auto report_start_cycles = read_cycle_counter();

  for (int i = 0; i < num_ops; ++i) {
    next_report--;
    if (next_report == 0) {
      next_report = reporting_interval;
      float rss_mb = get_rss_bytes() / 1024.0f / 1024.0f;

      auto report_end_inst = read_instret();
      auto report_end_cycles = read_cycle_counter();
      size_t insts = report_end_inst - report_start_inst;
      size_t cycles = report_end_cycles - report_start_cycles;

      report_start_inst = report_end_inst;
      report_start_cycles = report_end_cycles;

      printf("   ");
      printf("progress=%6.1f%%, ", (i + 1) * 100.0 / num_ops);
      printf("rss=%fmb, ", rss_mb);
      printf("C/OP=%10lf, ", cycles / (float)reporting_interval);
      printf("CPI=%5.2f, ", cycles / (insts + 1e-9f));
      printf("TPUT=%8zu, ", (size_t)(reporting_interval / (cycles / 1e9f)));
      printf("bads=%u, ", bads);
      printf("\n");
      fflush(stdout);
    }

    if (is_loading) {
      if (client.DoInsert()) {
        oks++;
      } else {
        bads++;
      }
    } else {
      // auto start = read_cycle_counter();
      if (client.DoTransaction()) {
        oks++;
      } else {
        bads++;
      }
      // auto end = read_cycle_counter();
    }
  }
  if (yukon_enable_localization != NULL) {
    yukon_enable_localization(false);
  }

  db->Close();

  // auto end_inst = read_instret();
  // printf("Done.\n");
  if (!is_loading) {
    change_roi(0);
    // printf("YUKON_END_CRITICAL=%zu\n", read_cycle_counter());
    // printf("YUKON_ROI_INSTRET=%zu\n", end_inst - start_inst);
  }
  return oks;
}

static int YCSBRun(RedisModuleCtx *ctx, RedisModuleString **argv,
                   int argc) {
  utils::Timer<double> timer;

  // extract argv into a std::vector<std::string>
  std::vector<const char *> args;
  if (argc != 2) {
    return RedisModule_WrongArity(ctx);
  }

  for (int i = 0; i < argc; ++i) {
    size_t len;
    const char *arg = RedisModule_StringPtrLen(argv[i], &len);
    args.push_back(arg);
  }

  utils::Properties props;
  for (auto &kv : props.properties()) {
    printf("Property: %s = %s\n", kv.first.c_str(), kv.second.c_str());
  }

  try {
    std::ifstream file(args[1]);
    props.Load(file);
  } catch (const std::exception &e) {
    return RedisModule_ReplyWithError(ctx, e.what());
  }

  if (std::stod(props.GetProperty("scanproportion", "0")) != 0) {
    return RedisModule_ReplyWithError(ctx, "ERR YCSB scans are not supported");
  }

  int insert_ops = stoi(props[ycsbc::CoreWorkload::RECORD_COUNT_PROPERTY]);
  int workload_ops = stoi(props[ycsbc::CoreWorkload::OPERATION_COUNT_PROPERTY]);
  if (insert_ops < 2 || workload_ops < 0) {
    return RedisModule_ReplyWithError(
        ctx, "ERR YCSB requires recordcount >= 2 and operationcount >= 0");
  }

  ycsbc::RedisInternalDB db(ctx);
  ycsbc::CoreWorkload wl;
  wl.Init(props);

  timer.Start();
  DelegateClient(&db, &wl, insert_ops, true);
  double insert_duration = timer.End();

  timer.Start();
  auto roi_start = read_cycle_counter();
  DelegateClient(&db, &wl, workload_ops, false);
  auto roi_end = read_cycle_counter();
  double workload_duration = timer.End();

  printf("YUKON_YCSB_ROI_CYCLES=%zu\n", roi_end - roi_start);
  printf("YUKON_YCSB_WORKLOAD=%s\n", args[1]);
  printf("YUKON_YCSB_INSERT_OPS=%d\n", insert_ops);
  printf("YUKON_YCSB_WORKLOAD_OPS=%d\n", workload_ops);
  printf("YUKON_YCSB_INSERT_DURATION=%f\n", insert_duration);
  printf("YUKON_YCSB_LOOKUP_DURATION=%f\n", workload_duration);
  printf("YUKON_YCSB_TPUT=%f\n", workload_ops / workload_duration);

  return RedisModule_ReplyWithSimpleString(ctx, "OK");
}

static int YCSBRun_RedisCommand(RedisModuleCtx *ctx, RedisModuleString **argv,
                               int argc) {
  try {
    return YCSBRun(ctx, argv, argc);
  } catch (const std::exception &e) {
    return RedisModule_ReplyWithError(ctx, e.what());
  } catch (...) {
    return RedisModule_ReplyWithError(ctx, "ERR YCSB workload failed");
  }
}

extern "C" {

int YCSB_OnLoad(RedisModuleCtx *ctx, RedisModuleString **argv, int argc) {
  REDISMODULE_NOT_USED(argv);
  REDISMODULE_NOT_USED(argc);
  if (RedisModule_Init(ctx, "ycsb", 1, REDISMODULE_APIVER_1) ==
      REDISMODULE_ERR) {
    return REDISMODULE_ERR;
  }

  if (RedisModule_CreateCommand(ctx, "ycsb.run", YCSBRun_RedisCommand, "admin write deny-oom",
                                0, 0, 0) == REDISMODULE_ERR) {
    return REDISMODULE_ERR;
  }

  return REDISMODULE_OK;
}
}
