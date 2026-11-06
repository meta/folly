/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <folly/concurrency/CacheLocality.h>

#ifndef _MSC_VER
#define _GNU_SOURCE 1 // for RTLD_NOLOAD
#include <dlfcn.h>
#endif
#include <string.h>

#include <filesystem>
#include <fstream>
#include <mutex>
#include <numeric>
#include <optional>
#include <unordered_map>

#include <fmt/core.h>
#include <glog/logging.h>
#include <folly/Indestructible.h>
#include <folly/Memory.h>
#include <folly/ScopeGuard.h>
#include <folly/container/Reserve.h>
#include <folly/hash/Hash.h>
#include <folly/lang/Exception.h>
#include <folly/portability/Fcntl.h>
#include <folly/portability/FmtCompile.h>
#include <folly/portability/Unistd.h>
#include <folly/system/ThreadId.h>

namespace folly {

///////////// CacheLocality

/// Returns the CacheLocality information best for this machine
CacheLocality CacheLocality::readSystemLocalityInfo() {
  if (kIsLinux) {
    if (kIsArchAmd64 || kIsArchX86) {
      // First try to parse /proc/cpuinfo.
      // But only on arch's where the file has cpu topology hints.
      // If that fails, then try to parse /sys/devices/.
      // The latter is slower but more accurate.
      try {
        return CacheLocality::readFromProcCpuinfo();
      } catch (...) {
        // /proc/cpuinfo might be non-standard
        // lets try with sysfs /sys/devices/cpu
      }
    }

    try {
      return CacheLocality::readFromSysfs();
    } catch (...) {
      // keep trying
    }
  }

  long numCpus = sysconf(_SC_NPROCESSORS_CONF);
  if (numCpus <= 0) {
    // This shouldn't happen, but if it does we should try to keep
    // going.  We are probably not going to be able to parse /sys on
    // this box either (although we will try), which means we are going
    // to fall back to the SequentialThreadId splitter.  On my 16 core
    // (x hyperthreading) dev box 16 stripes is enough to get pretty good
    // contention avoidance with SequentialThreadId, and there is little
    // improvement from going from 32 to 64.  This default gives us some
    // wiggle room
    numCpus = 32;
  }
  return CacheLocality::uniform(size_t(numCpus));
}

template <>
const CacheLocality& CacheLocality::system<std::atomic>() {
  static std::atomic<const CacheLocality*> cache;
  auto value = cache.load(std::memory_order_acquire);
  if (value != nullptr) {
    return *value;
  }
  auto next = new CacheLocality(readSystemLocalityInfo());
  if (cache.compare_exchange_strong(value, next, std::memory_order_acq_rel)) {
    return *next;
  }
  delete next;
  return *value;
}

CacheLocality::CacheLocality(std::vector<std::vector<size_t>> equivClasses) {
  numCpus = equivClasses.size();

  for (size_t cpu = 0; cpu < numCpus; ++cpu) {
    for (size_t level = 0; level < equivClasses[cpu].size(); ++level) {
      if (equivClasses[cpu][level] == cpu) {
        // we only want to count the equiv classes once, so we do it when we
        // are processing their representative.
        while (numCachesByLevel.size() <= level) {
          numCachesByLevel.push_back(0);
        }
        numCachesByLevel[level]++;
      }
    }
  }

  std::vector<size_t> cpus(numCpus);
  std::iota(cpus.begin(), cpus.end(), 0);

  std::sort(cpus.begin(), cpus.end(), [&](size_t lhs, size_t rhs) -> bool {
    auto& lhsEquiv = equivClasses[lhs];
    auto& rhsEquiv = equivClasses[rhs];

    // If different cpus have different numbers of caches group first by number
    // of caches to guarantee strict weak ordering, even though the resulting
    // order may be sub-optimal.
    if (lhsEquiv.size() != rhsEquiv.size()) {
      return lhsEquiv.size() < rhsEquiv.size();
    }

    // Order by equiv class of cache with highest index, direction doesn't
    // matter.
    for (size_t i = lhsEquiv.size(); i > 0; --i) {
      auto idx = i - 1;
      if (lhsEquiv[idx] != rhsEquiv[idx]) {
        return lhsEquiv[idx] < rhsEquiv[idx];
      }
    }

    // Break ties deterministically by cpu.
    return lhs < rhs;
  });

  // The cpus are now sorted by locality, with neighboring entries closer
  // to each other than entries that are far away.  For striping we want
  // the inverse map, since we are starting with the cpu.
  localityIndexByCpu.resize(numCpus);
  for (size_t i = 0; i < cpus.size(); ++i) {
    localityIndexByCpu[cpus[i]] = i;
  }

  equivClassesByCpu = std::move(equivClasses);
}

// Each level of cache has sharing sets, which are the set of cpus that share a
// common cache at that level.  These are available in a hex bitset form
// (/sys/devices/system/cpu/cpu0/cache/index0/shared_cpu_map, for example).
// They are also available in human-readable form in the shared_cpu_list file in
// the same directory.  The list is a comma-separated list of numbers and
// ranges, where the ranges are pairs of decimal numbers separated by a '-'.
//
// To sort the cpus for optimum locality we don't really need to parse the
// sharing sets, we just need a unique representative from the equivalence
// class.  The smallest value works fine, and happens to be the first decimal
// number in the file.  We load all of the equivalence class information from
// all of the cpu*/index* directories, order the cpus first by increasing
// last-level cache equivalence class, then by the smaller caches.  Finally, we
// break ties with the cpu number itself.

/// Returns the first decimal number in the line, or throws an exception if the
/// line does not start with a number terminated by ',', '-', '\n', or EOS.
static size_t parseLeadingNumber(const std::string& line) {
  auto raw = line.c_str();
  char* end;
  unsigned long val = strtoul(raw, &end, 10);
  if (end == raw || (*end != ',' && *end != '-' && *end != '\n' && *end != 0)) {
    throw std::runtime_error(fmt::format("error parsing list '{}'", line));
  }
  return val;
}

CacheLocality CacheLocality::readFromSysfsTree(
    [[maybe_unused]] std::string_view root) {
#if defined(_WIN32)
  // windows does not have openat and open flag constants
  return CacheLocality::uniform(0);
#else

  // the list of cache equivalence classes, where equivalence classes
  // are named by the smallest cpu in the class
  std::vector<std::vector<size_t>> equivClassesByCpu;

  auto checkNoEnt = [](std::string_view name) {
    auto err = errno;
    if (err != ENOENT) {
      throw std::runtime_error(
          fmt::format(
              "unexpected error while opening {}: {}", name, strerror(err)));
    }
  };

  // Reads the first 64 bytes of the file.
  auto rdfile64 = [&](int dirfd, const std::string& name) {
    const auto fd = ::openat(dirfd, name.c_str(), O_CLOEXEC, O_RDONLY);
    if (fd < 0) {
      checkNoEnt(name);
      return std::string(); // stop condition for the inner loop below
    }
    SCOPE_EXIT {
      ::close(fd);
    };

    alignas(64) char buf[64];
    int ret = 0;
    do {
      ret = ::pread(fd, buf, sizeof(buf), 0);
    } while (ret < 0 && errno == EINTR);
    if (ret < 0) {
      return std::string();
    }
    return std::string(buf, to_unsigned(ret));
  };

  auto subroot = std::filesystem::path(root) / "sys/devices/system/cpu";
  const int allfd = ::open(subroot.c_str(), O_DIRECTORY | O_CLOEXEC, O_RDONLY);
  if (allfd < 0) {
    auto err = errno;
    throw std::runtime_error(
        fmt::format("unable to open sysfs: {}", strerror(err)));
  }
  SCOPE_EXIT {
    ::close(allfd);
  };

  size_t maxindex = 0;
  for (size_t cpu = 0;; ++cpu) {
    auto cpuroot = fmt::format(FMT_COMPILE("cpu{}/cache"), cpu);
    const int cpufd =
        ::openat(allfd, cpuroot.c_str(), O_DIRECTORY | O_CLOEXEC, O_RDONLY);
    if (cpufd < 0) {
      checkNoEnt(cpuroot);
      break;
    }
    SCOPE_EXIT {
      ::close(cpufd);
    };

    std::vector<size_t> levels;
    grow_capacity_by(levels, maxindex);
    for (size_t index = 0;; ++index) {
      auto dir = fmt::format(FMT_COMPILE("index{}/"), index);
      auto cacheType = rdfile64(cpufd, dir + "type");
      if (cacheType.empty()) {
        // no more caches
        break;
      }
      if (cacheType[0] == 'I') {
        // cacheType in { "Data", "Instruction", "Unified" }. skip icache
        continue;
      }
      // only try to read the second file once we know we will need it
      auto equivStr = rdfile64(cpufd, dir + "shared_cpu_list");
      if (equivStr.empty()) {
        // no more caches
        break;
      }
      auto equiv = parseLeadingNumber(equivStr);
      levels.push_back(equiv);
    }
    maxindex = std::max(maxindex, levels.size());

    if (levels.empty()) {
      // no levels at all for this cpu, we must be done
      break;
    }
    equivClassesByCpu.emplace_back(std::move(levels));
  }

  if (equivClassesByCpu.empty()) {
    throw std::runtime_error("unable to load cache sharing info");
  }

  return CacheLocality{std::move(equivClassesByCpu)};

#endif
}

CacheLocality CacheLocality::readFromSysfs() {
  return readFromSysfsTree();
}

namespace {

static bool procCpuinfoLineRelevant(std::string const& line) {
  return line.size() > 4 && (line[0] == 'p' || line[0] == 'c');
}

std::vector<std::tuple<size_t, size_t, size_t>> parseProcCpuinfoLines(
    std::vector<std::string> const& lines) {
  std::vector<std::tuple<size_t, size_t, size_t>> cpus;
  size_t physicalId = 0;
  size_t coreId = 0;
  size_t maxCpu = 0;
  size_t numberOfPhysicalIds = 0;
  size_t numberOfCoreIds = 0;
  for (auto iter = lines.rbegin(); iter != lines.rend(); ++iter) {
    auto& line = *iter;
    if (!procCpuinfoLineRelevant(line)) {
      continue;
    }

    auto sepIndex = line.find(':');
    if (sepIndex == std::string::npos || sepIndex + 2 > line.size()) {
      continue;
    }
    auto arg = line.substr(sepIndex + 2);

    // "physical id" is socket, which is the most important locality
    // context.  "core id" is a real core, so two "processor" entries with
    // the same physical id and core id are hyperthreads of each other.
    // "processor" is the top line of each record, so when we hit it in
    // the reverse order then we can emit a record.
    if (line.find("physical id") == 0) {
      physicalId = parseLeadingNumber(arg);
      ++numberOfPhysicalIds;
    } else if (line.find("core id") == 0) {
      coreId = parseLeadingNumber(arg);
      ++numberOfCoreIds;
    } else if (line.find("processor") == 0) {
      auto cpu = parseLeadingNumber(arg);
      maxCpu = std::max(cpu, maxCpu);
      cpus.emplace_back(physicalId, coreId, cpu);
    }
  }

  if (cpus.empty()) {
    throw std::runtime_error("no CPUs parsed from /proc/cpuinfo");
  }
  if (maxCpu != cpus.size() - 1) {
    throw std::runtime_error(
        "offline CPUs not supported for /proc/cpuinfo cache locality source");
  }
  if (numberOfPhysicalIds == 0) {
    throw std::runtime_error("no physical ids found");
  }
  if (numberOfCoreIds == 0) {
    throw std::runtime_error("no core ids found");
  }

  return cpus;
}

} // namespace

CacheLocality CacheLocality::readFromProcCpuinfoLines(
    std::vector<std::string> const& lines) {
  // (physicalId, coreId, cpu)
  std::vector<std::tuple<size_t, size_t, size_t>> cpus =
      parseProcCpuinfoLines(lines);
  // Sort to make equivalence classes contiguous.
  std::sort(cpus.begin(), cpus.end());

  // We can't tell the real cache hierarchy from /proc/cpuinfo, but it works
  // well enough to assume there are 3 levels, L1 and L2 per-core and L3 per
  // socket.
  std::vector<std::vector<size_t>> equivClassesByCpu(cpus.size());
  size_t l1Equiv = 0;
  size_t l3Equiv = 0;
  for (size_t i = 0; i < cpus.size(); ++i) {
    auto [physicalId, coreId, cpu] = cpus[i];
    // The representative for each L1 and L3 equivalence class is the first cpu
    // in the class.
    if (i == 0 || physicalId != std::get<0>(cpus[i - 1]) ||
        coreId != std::get<1>(cpus[i - 1])) {
      l1Equiv = cpu;
    }
    if (i == 0 || physicalId != std::get<0>(cpus[i - 1])) {
      l3Equiv = cpu;
    }
    equivClassesByCpu[cpu] = {l1Equiv, l1Equiv, l3Equiv};
  }

  return CacheLocality{std::move(equivClassesByCpu)};
}

CacheLocality CacheLocality::readFromProcCpuinfo() {
  std::vector<std::string> lines;
  {
    std::ifstream xi("/proc/cpuinfo");
    if (xi.fail()) {
      throw std::runtime_error("unable to open /proc/cpuinfo");
    }
    char buf[8192];
    while (xi.good() && lines.size() < 20000) {
      xi.getline(buf, sizeof(buf));
      std::string str(buf);
      if (procCpuinfoLineRelevant(str)) {
        lines.emplace_back(std::move(str));
      }
    }
  }
  return readFromProcCpuinfoLines(lines);
}

CacheLocality CacheLocality::uniform(size_t numCpus) {
  // One cache shared by all cpus.
  std::vector<std::vector<size_t>> equivClassesByCpu(numCpus, {0});
  return CacheLocality{std::move(equivClassesByCpu)};
}

////////////// Getcpu

Getcpu::Func Getcpu::resolveVdsoFunc() {
#if !defined(FOLLY_HAVE_LINUX_VDSO) || defined(FOLLY_SANITIZE_MEMORY)
  return nullptr;
#else
  void* h = dlopen("linux-vdso.so.1", RTLD_LAZY | RTLD_LOCAL | RTLD_NOLOAD);
  if (h == nullptr) {
    return nullptr;
  }

  auto func = Getcpu::Func(dlsym(h, "__vdso_getcpu"));
  if (func == nullptr) {
    // technically a null result could either be a failure or a successful
    // lookup of a symbol with the null value, but the second can't actually
    // happen for this symbol.  No point holding the handle forever if
    // we don't need the code
    dlclose(h);
  }

  return func;
#endif
}

/////////////// SequentialThreadId
uint64_t SequentialThreadId::get() {
  return get_unique_thread_id();
}

/////////////// HashingThreadId
unsigned HashingThreadId::get() {
  return hash::twang_32from64(getCurrentThreadID());
}

namespace detail {

int AccessSpreaderBase::degenerateGetcpu(unsigned* cpu, unsigned* node, void*) {
  if (cpu != nullptr) {
    *cpu = 0;
  }
  if (node != nullptr) {
    *node = 0;
  }
  return 0;
}

struct AccessSpreaderStaticInit {
  static AccessSpreaderStaticInit instance;
  AccessSpreaderStaticInit() { (void)AccessSpreader<>::current(~size_t(0)); }
};
AccessSpreaderStaticInit AccessSpreaderStaticInit::instance;

bool AccessSpreaderBase::initialize(
    GlobalState& state,
    Getcpu::Func (&pickGetcpuFunc)(),
    const CacheLocality& (&system)()) {
  (void)AccessSpreaderStaticInit::instance; // ODR-use it so it is not dropped
  constexpr auto relaxed = std::memory_order_relaxed;
  auto& cacheLocality = system();
  auto n = cacheLocality.numCpus;
  for (size_t width = 0; width <= kMaxCpus; ++width) {
    auto& row = state.table[width];
    auto numStripes = std::max(size_t{1}, width);
    for (size_t cpu = 0; cpu < kMaxCpus && cpu < n; ++cpu) {
      auto index = cacheLocality.localityIndexByCpu[cpu];
      assert(index < n);
      // as index goes from 0..n, post-transform value goes from
      // 0..numStripes
      make_atomic_ref(row[cpu]).store(
          static_cast<CompactStripe>((index * numStripes) / n), relaxed);
      assert(make_atomic_ref(row[cpu]).load(relaxed) < numStripes);
    }
    size_t filled = n;
    while (filled < kMaxCpus) {
      size_t len = std::min(filled, kMaxCpus - filled);
      for (size_t i = 0; i < len; ++i) {
        make_atomic_ref(row[filled + i])
            .store(make_atomic_ref(row[i]).load(relaxed), relaxed);
      }
      filled += len;
    }
    for (size_t cpu = n; cpu < kMaxCpus; ++cpu) {
      assert(
          make_atomic_ref(row[cpu]).load(relaxed) ==
          make_atomic_ref(row[cpu - n]).load(relaxed));
    }
  }
  state.getcpu.exchange(pickGetcpuFunc(), std::memory_order_acq_rel);
  return true;
}

} // namespace detail

/* static */ LLCAccessSpreader& LLCAccessSpreader::get() {
  static folly::Indestructible<LLCAccessSpreader> instance(PrivateTag{});
  return *instance;
}

LLCAccessSpreader::LLCAccessSpreader(PrivateTag) {
#ifdef __linux__
  auto getcpu = Getcpu::resolveVdsoFunc();
  getcpu_ = getcpu ? getcpu : &FallbackGetcpuType::getcpu;

  // /proc/cpuinfo does not have accurate LLC info, we need to force a read
  // from sysfs.
  auto cl = CacheLocality::readFromSysfs();
  stripeByCpu_.resize(cl.numCpus);

  // Only have stripes for LLCs that are accessible from the current process,
  // and number them sequentially as we discover them.
  std::unordered_map<size_t, size_t> cacheIdx;
  cpu_set_t cpuset;
  PCHECK(sched_getaffinity(0, sizeof(cpuset), &cpuset) == 0);
  for (size_t cpu = 0; cpu < cl.numCpus; ++cpu) {
    if (!CPU_ISSET(cpu, &cpuset)) {
      continue;
    }

    auto llcEquiv = cl.equivClassesByCpu[cpu].back();
    auto [it, _] = cacheIdx.try_emplace(llcEquiv, cacheIdx.size());
    stripeByCpu_[cpu] = it->second;
  }

  numStripes_ = cacheIdx.size();
#else
  numStripes_ = 1;
  (void)getcpu_;
  (void)stripeByCpu_;
#endif // __linux__
}

size_t LLCAccessSpreader::current() const {
#ifdef __linux__
  struct ThreadCache {
    size_t usesLeft = 0;
    size_t value;
  };

  // We expect that a thread doesn't migrate LLC often, so reuse the value a
  // few times before refreshing it.
  // TODO(ott): Re-evaluate once we can use rseq to get the current CPU.
  thread_local ThreadCache tc;
  if (tc.usesLeft-- == 0) {
    tc.usesLeft = 16; // A small number is enough to amortize.
    unsigned cpu;
    getcpu_(&cpu, nullptr, nullptr);
    // If the set of active CPUs can change at runtime just return the 0
    // stripe. This should not happen in normal operations.
    tc.value = cpu < stripeByCpu_.size() ? stripeByCpu_[cpu] : 0;
  }

  return tc.value;
#else
  return 0;
#endif // __linux__
}

size_t LLCAccessSpreader::numStripes() const {
  return numStripes_;
}

namespace {

/**
 * A simple freelist allocator.  Allocates things of size sz, from slabs of size
 * kAllocSize.  Takes a lock on each allocation/deallocation.
 */
class SimpleAllocator {
 public:
  // To support array aggregate initialization without an implicit constructor.
  struct Ctor {};

  SimpleAllocator(Ctor, size_t sz) : sz_(sz) {
    static_assert(
        sizeof(void*) <= 64,
        "SimpleAllocator assumes sizeof(void*) fits in maximum size class");
    if (sz_ < sizeof(void*)) {
      folly::throw_exception<std::invalid_argument>(fmt::format(
          "SimpleAllocator size {} is too small (minimum: {})",
          sz_,
          sizeof(void*)));
    }
  }

  ~SimpleAllocator() {
    std::lock_guard g(m_);
    for (auto& block : blocks_) {
      folly::aligned_free(block);
    }
  }

  SimpleAllocator(const SimpleAllocator&) = delete;
  SimpleAllocator& operator=(const SimpleAllocator&) = delete;
  SimpleAllocator(SimpleAllocator&&) = delete;
  SimpleAllocator& operator=(SimpleAllocator&&) = delete;

  void* allocate() {
    std::lock_guard g(m_);
    // Freelist allocation.
    if (freelist_) {
      auto mem = freelist_;
      freelist_ = *static_cast<void**>(freelist_);
      return mem;
    }

    if (mem_) {
      // Bump-ptr allocation.
      if (intptr_t(mem_) % kMallocAlign == 0) {
        // Avoid allocating pointers that may look like malloc
        // pointers.
        mem_ += std::min(sz_, max_align_v);
      }
      if (mem_ + sz_ <= end_) {
        auto mem = mem_;
        mem_ += sz_;

        assert(intptr_t(mem) % kMallocAlign != 0);
        return mem;
      }
    }

    return allocateHard();
  }

  static void deallocate(void* ptr) {
    assert(intptr_t(ptr) % kMallocAlign != 0);
    // Find the allocator instance.
    auto addr =
        reinterpret_cast<void*>(intptr_t(ptr) & ~intptr_t(kAllocSize - 1));
    auto allocator = *static_cast<SimpleAllocator**>(addr);

    std::lock_guard g(allocator->m_);
    *static_cast<void**>(ptr) = allocator->freelist_;
    if constexpr (kIsSanitizeAddress) {
      // If running under ASAN, scrub the memory on deallocation, so we don't
      // leave pointers that could hide leaks at shutdown, since the backing
      // slabs may not be deallocated if the instance is a leaky singleton.
      auto* base = static_cast<char*>(ptr);
      std::fill(
          base + sizeof(void*), base + allocator->sz_, static_cast<char>(0));
    }
    allocator->freelist_ = ptr;
  }

  constexpr static size_t kMallocAlign =
      std::max(size_t(128), hardware_destructive_interference_size);
  static_assert(
      kMallocAlign % hardware_destructive_interference_size == 0,
      "Large allocations should be cacheline-aligned");

 private:
  constexpr static size_t kAllocSize = 4096;

  void* allocateHard() {
    // Allocate a new slab.
    mem_ = static_cast<uint8_t*>(folly::aligned_malloc(kAllocSize, kAllocSize));
    if (!mem_) {
      throw_exception<std::bad_alloc>();
    }
    end_ = mem_ + kAllocSize;
    blocks_.push_back(mem_);

    // Install a pointer to ourselves as the allocator.
    *reinterpret_cast<SimpleAllocator**>(mem_) = this;
    static_assert(
        max_align_v >= sizeof(SimpleAllocator*), "alignment too small");
    mem_ += std::min(sz_, max_align_v);

    // New allocation.
    auto mem = mem_;
    mem_ += sz_;
    assert(intptr_t(mem) % kMallocAlign != 0);
    return mem;
  }

  std::mutex m_;
  uint8_t* mem_{nullptr};
  uint8_t* end_{nullptr};
  void* freelist_{nullptr};
  size_t sz_;
  std::vector<void*> blocks_;
};

class Allocator {
 public:
  void* allocate(size_t size) {
    if (auto cl = sizeClass(size)) {
      return allocators_[*cl].allocate();
    }

    // Fall back to malloc, returning a kMallocAlign-aligned allocation so it
    // can be distinguished from SimpleAllocator allocations.
    size = size + (SimpleAllocator::kMallocAlign - 1);
    size &= ~size_t(SimpleAllocator::kMallocAlign - 1);
    void* mem = aligned_malloc(size, SimpleAllocator::kMallocAlign);
    if (!mem) {
      throw_exception<std::bad_alloc>();
    }
    return mem;
  }

  static void deallocate(void* ptr) {
    if (!ptr) {
      return;
    }

    // See if it came from SimpleAllocator or malloc.
    if (intptr_t(ptr) % SimpleAllocator::kMallocAlign != 0) {
      SimpleAllocator::deallocate(ptr);
    } else {
      aligned_free(ptr);
    }
  }

 private:
  std::optional<uint8_t> sizeClass(size_t size) {
    if (size <= 8) {
      return 0;
    } else if (size <= 16) {
      return 1;
    } else if (size <= 32) {
      return 2;
    } else if (size <= 64) {
      return 3;
    } else {
      return std::nullopt;
    }
  }

  std::array<SimpleAllocator, 4> allocators_{
      {{SimpleAllocator::Ctor{}, 8},
       {SimpleAllocator::Ctor{}, 16},
       {SimpleAllocator::Ctor{}, 32},
       {SimpleAllocator::Ctor{}, 64}}};
};

} // namespace

void* coreMalloc(size_t size, size_t numStripes, size_t stripe) {
  static folly::Indestructible<Allocator>
      allocators[AccessSpreader<>::maxLocalityIndexValue()];
  auto index = AccessSpreader<>::localityIndexForStripe(numStripes, stripe);
  return allocators[index]->allocate(size);
}

void coreFree(void* ptr) {
  Allocator::deallocate(ptr);
}

namespace {
thread_local CoreAllocatorGuard* gCoreAllocatorGuard = nullptr;
}

CoreAllocatorGuard::CoreAllocatorGuard(size_t numStripes, size_t stripe)
    : numStripes_(numStripes), stripe_(stripe) {
  CHECK(gCoreAllocatorGuard == nullptr)
      << "CoreAllocator::Guard cannot be used recursively";
  gCoreAllocatorGuard = this;
}

CoreAllocatorGuard::~CoreAllocatorGuard() {
  gCoreAllocatorGuard = nullptr;
}

namespace detail {

void* coreMallocFromGuard(size_t size) {
  CHECK(gCoreAllocatorGuard != nullptr)
      << "CoreAllocator::allocator called without an active Guard";
  return coreMalloc(
      size, gCoreAllocatorGuard->numStripes_, gCoreAllocatorGuard->stripe_);
}

} // namespace detail

} // namespace folly
