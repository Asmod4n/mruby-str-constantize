#include <mruby/str_constantize.h>

#include <mruby.h>
#include <mruby/class.h>
#include <mruby/hash.h>
#include <mruby/string.h>
#include <mruby/variable.h>
#include <mruby/presym.h>
#include <mruby/branch_pred.h>
#include <mruby/cpp_helpers.hpp>

#include <cstring>
#include <string>
#include <string_view>
#include <vector>
#include <limits>
#include <cstdint>
#include <ctime>

// ============================================================================
// LFU cache
// ============================================================================

class ClassCacheLfu {
public:
  static constexpr uint16_t NIL       = UINT16_MAX;
  static constexpr uint16_t MAX_FREQ  = 255;
  static constexpr uint16_t MAX_SIZE  = 128;
  static constexpr uint16_t INDEX_CAP = 256;
  static constexpr uint16_t KEY_MAX   = 64;

  struct Entry {
    char     key[KEY_MAX];
    uint8_t  key_len;
    uint16_t freq;
    uint16_t prev;
    uint16_t next;
  };

  struct Bucket {
    uint16_t head = NIL;
    uint16_t tail = NIL;
  };

  struct Slot {
    uint32_t hash = 0;
    uint16_t idx  = NIL;
    bool     used = false;
  };

  Entry    entries[MAX_SIZE]{};
  uint16_t count = 0;

  Bucket   buckets[MAX_FREQ + 1]{};
  uint16_t min_freq = 1;

  Slot     index[INDEX_CAP]{};

  uint16_t last_evicted_idx = NIL;
  bool     had_eviction     = false;

  /* Per-instance seed for hash-flooding resistance. Mixed into FNV-1a's
   * basis so an attacker cannot precompute a colliding set of class
   * names across process boundaries. Not crypto-strong; not meant to be. */
  uint32_t seed = 0;

  ClassCacheLfu()
  {
    seed = (uint32_t)std::time(nullptr)
         ^ (uint32_t)(uintptr_t)this
         ^ 0x9e3779b9u;
    if (seed == 0) seed = 0xa5a5a5a5u;
  }
  ~ClassCacheLfu() = default;

  void touch(uint16_t idx)
  {
    uint16_t old_freq = entries[idx].freq;
    bucket_remove(idx);

    if (buckets[old_freq].head == NIL && old_freq == min_freq) {
      if (min_freq < MAX_FREQ) min_freq++;
    }

    uint16_t new_freq = old_freq < MAX_FREQ ? old_freq + 1 : MAX_FREQ;
    bucket_insert(idx, new_freq);
  }

  void insert(const char *key_ptr, uint16_t key_len)
  {
    if (key_len > KEY_MAX) return;

    uint16_t idx;
    if (count < MAX_SIZE) {
      idx = count++;
    } else {
      idx = evict_one();
      if (idx == NIL) {
        mrb_assert(false && "LFU evict_one() returned NIL in insert");
        return;
      }
    }

    Entry &e = entries[idx];
    std::memcpy(e.key, key_ptr, key_len);
    e.key_len = (uint8_t)key_len;
    e.freq    = 1;
    e.prev    = NIL;
    e.next    = NIL;
    min_freq  = 1;

    bucket_insert(idx, 1);
    index_set(e, idx);
  }

  uint16_t find(const char *key_ptr, uint16_t key_len) const
  {
    if (key_len > KEY_MAX) return NIL;

    uint32_t h    = hash(key_ptr, key_len);
    uint16_t mask = INDEX_CAP - 1;

    for (uint16_t i = 0; i < INDEX_CAP; ++i) {
      uint16_t slot      = (h + i) & mask;
      const Slot &s = index[slot];

      if (!s.used) return NIL;
      if (s.hash == h) {
        const Entry &e = entries[s.idx];
        if (e.key_len == key_len &&
            std::memcmp(e.key, key_ptr, key_len) == 0)
          return s.idx;
      }
    }
    return NIL;
  }

  void evict(mrb_state *mrb, mrb_value class_cache)
  {
    if (!had_eviction || last_evicted_idx == NIL) return;

    Entry &e        = entries[last_evicted_idx];
    mrb_value key   = mrb_str_new_static(mrb, e.key, e.key_len);
    mrb_hash_delete_key(mrb, class_cache, key);

    had_eviction     = false;
    last_evicted_idx = NIL;
  }

private:
  uint32_t hash(const char *p, uint16_t len) const
  {
    uint32_t h = 2166136261u ^ seed;
    for (uint16_t i = 0; i < len; ++i) {
      h ^= static_cast<unsigned char>(p[i]);
      h *= 16777619u;
    }
    return h;
  }

  void index_set(const Entry &e, uint16_t idx)
  {
    uint32_t h    = hash(e.key, e.key_len);
    uint16_t mask = INDEX_CAP - 1;

    /* MAX_SIZE (128) < INDEX_CAP (256): a free slot must exist. */
    for (uint16_t i = 0; i < INDEX_CAP; ++i) {
      uint16_t slot = (h + i) & mask;
      Slot &s = index[slot];

      if (!s.used || (s.hash == h && s.idx == idx)) {
        s.used = true;
        s.hash = h;
        s.idx  = idx;
        return;
      }
    }
    mrb_assert(false && "index_set: probe chain exhausted");
  }

  void index_erase(const Entry &e)
  {
    uint32_t h_target           = hash(e.key, e.key_len);
    uint16_t mask               = INDEX_CAP - 1;
    uint16_t target_idx_entries = (uint16_t)(&e - entries);
    uint16_t empty_slot         = NIL;

    /* Locate the slot. */
    for (uint16_t i = 0; i < INDEX_CAP; ++i) {
      uint16_t slot = (h_target + i) & mask;
      Slot &s = index[slot];
      if (!s.used) return;
      if (s.hash == h_target && s.idx == target_idx_entries) {
        s.used     = false;
        empty_slot = slot;
        break;
      }
    }
    if (empty_slot == NIL) return;

    /* Backshift compaction. While the slot after empty is occupied and
     * the entry there could legally live in empty_slot (its natural
     * position is at-or-before empty on the probe path), move it.
     * Preserves the invariant that every entry is reachable from its
     * natural slot without crossing an unused slot. Without this,
     * find() would short-circuit at the erased slot and miss entries
     * that collided past it. */
    for (uint16_t i = 0; i < INDEX_CAP; ++i) {
      uint16_t next_slot = (uint16_t)((empty_slot + 1) & mask);
      Slot &next = index[next_slot];
      if (!next.used) return;

      uint16_t natural = (uint16_t)(next.hash & mask);
      uint16_t d_empty = (uint16_t)((empty_slot - natural) & mask);
      uint16_t d_next  = (uint16_t)((next_slot  - natural) & mask);

      if (d_empty <= d_next) {
        index[empty_slot] = next;
        next.used  = false;
        empty_slot = next_slot;
      } else {
        return;
      }
    }
  }

  void bucket_remove(uint16_t idx)
  {
    Entry  &e = entries[idx];
    Bucket &b = buckets[e.freq];

    if (e.prev != NIL) entries[e.prev].next = e.next;
    else               b.head = e.next;

    if (e.next != NIL) entries[e.next].prev = e.prev;
    else               b.tail = e.prev;

    e.prev = e.next = NIL;
  }

  void bucket_insert(uint16_t idx, uint16_t freq)
  {
    Bucket &b = buckets[freq];
    Entry  &e = entries[idx];

    e.freq = freq;
    e.prev = b.tail;
    e.next = NIL;

    if (b.tail != NIL) entries[b.tail].next = idx;
    else               b.head = idx;

    b.tail = idx;
  }

  uint16_t evict_one()
  {
    uint16_t f = min_freq;
    while (f <= MAX_FREQ && buckets[f].head == NIL) ++f;
    if (f > MAX_FREQ) return NIL;

    min_freq     = f;
    uint16_t idx = buckets[min_freq].head;
    if (idx == NIL) return NIL;

    bucket_remove(idx);
    index_erase(entries[idx]);

    last_evicted_idx = idx;
    had_eviction     = true;
    return idx;
  }
};

MRB_CPP_DEFINE_TYPE(ClassCacheLfu, str_constantize_lfu)

// ============================================================================
// GV names — private to this gem, no msgpack dependency
// ============================================================================

static ClassCacheLfu *
ensure_lfu(mrb_state *mrb)
{
  mrb_value obj = mrb_gv_get(mrb, MRB_SYM(__str_constantize_lfu__));
  if (likely(mrb_data_p(obj))) {
    return mrb_cpp_get<ClassCacheLfu>(mrb, obj);
  }

  // First call: also initialise the hash cache
  mrb_value cache = mrb_hash_new_capa(mrb, 8);
  mrb_gv_set(mrb, MRB_SYM(__str_constantize_cache__), cache);

  struct RClass *lfu_class =
    mrb_define_class_id(mrb, MRB_SYM(__StrConstantizeLfu), mrb->object_class);
  MRB_SET_INSTANCE_TT(lfu_class, MRB_TT_DATA);

  mrb_define_method_id(mrb, lfu_class, MRB_SYM(initialize),
    [](mrb_state *mrb, mrb_value self) -> mrb_value {
      mrb_cpp_new<ClassCacheLfu>(mrb, self);
      return self;
    },
    MRB_ARGS_NONE());

  obj = mrb_obj_new(mrb, lfu_class, 0, NULL);
  mrb_gv_set(mrb, MRB_SYM(__str_constantize_lfu__), obj);
  return mrb_cpp_get<ClassCacheLfu>(mrb, obj);
}

// ============================================================================
// Core lookup (slow path — no cache hit)
// ============================================================================

static mrb_value
do_constantize(mrb_state *mrb, mrb_value str,
               mrb_value cache, ClassCacheLfu *lfu)
{
  using std::string_view;

  const char *ptr = RSTRING_PTR(str);
  mrb_int     len = RSTRING_LEN(str);
  string_view full(ptr, len);

  auto name_error = [&]() {
    mrb_raisef(mrb, E_NAME_ERROR, "wrong constant name %v", str);
  };

  if (unlikely(full.empty() || full == "::")) name_error();

  mrb_value current = mrb_obj_value(mrb->object_class);

  if (full.size() >= 2 && full.substr(0, 2) == "::") {
    full.remove_prefix(2);
    if (unlikely(full.empty())) name_error();
  }

  // Split on "::"
  std::vector<string_view> segments;
  size_t start = 0;
  while (start <= full.size()) {
    size_t pos = full.find("::", start);
    if (pos == string_view::npos) {
      segments.emplace_back(full.substr(start));
      break;
    }
    segments.emplace_back(full.substr(start, pos - start));
    start = pos + 2;
  }

  for (auto seg : segments)
    if (unlikely(seg.empty())) name_error();

  for (size_t i = 0; i < segments.size(); ++i) {
    string_view seg = segments[i];
    /* mrb_intern_check: returns 0 if the symbol was never interned. Since
     * any defined constant must have an interned name in the symbol table,
     * sym == 0 proves the constant cannot exist. Failing here avoids
     * leaking attacker-controlled bytes into the (monotonic) symbol table. */
    mrb_sym sym = mrb_intern_check(mrb, seg.data(), (mrb_int)seg.size());

    if (unlikely(sym == 0 || !mrb_const_defined_at(mrb, current, sym)))
      mrb_raisef(mrb, E_NAME_ERROR, "uninitialized constant %v", str);

    mrb_value cnst = mrb_const_get(mrb, current, sym);

    bool last = (i + 1 == segments.size());
    if (!last) {
      enum mrb_vtype t = mrb_type(cnst);
      bool ok = (t == MRB_TT_CLASS  || t == MRB_TT_MODULE ||
                 t == MRB_TT_SCLASS || t == MRB_TT_ICLASS);
      if (unlikely(!ok))
        mrb_raisef(mrb, E_TYPE_ERROR,
                   "%v does not refer to a class/module",
                   mrb_str_new(mrb, seg.data(), (mrb_int)seg.size()));
      current = cnst;
    } else {
      mrb_hash_set(mrb, cache, str, cnst);
      lfu->insert(ptr, (uint16_t)len);
      lfu->evict(mrb, cache);
      return cnst;
    }
  }

  return current; // unreachable
}

static mrb_value
mrb_str_constantize_m(mrb_state *mrb, mrb_value self)
{
  return mrb_str_constantize(mrb, self);
}

static mrb_value
mrb_str_constantize_cache_clear_m(mrb_state *mrb, mrb_value self)
{
  (void)self;
  mrb_str_constantize_cache_clear(mrb);
  return mrb_nil_value();
}

// ============================================================================
// Public API
// ============================================================================

MRB_BEGIN_DECL

MRB_API mrb_value
mrb_str_constantize(mrb_state *mrb, mrb_value str)
{
  if (unlikely(!mrb_string_p(str)))
    mrb_raise(mrb, E_TYPE_ERROR, "constant name must be a String");

  const char *ptr = RSTRING_PTR(str);
  mrb_int     len = RSTRING_LEN(str);

  ClassCacheLfu *lfu = ensure_lfu(mrb);
  mrb_value   cache = mrb_gv_get(mrb, MRB_SYM(__str_constantize_cache__));

  // Fast path: hash cache hit
  mrb_value cached = mrb_hash_get(mrb, cache, str);
  if (mrb_class_p(cached)) {
    uint16_t idx = lfu->find(ptr, (uint16_t)len);
    if (idx != ClassCacheLfu::NIL) lfu->touch(idx);
    else                           lfu->insert(ptr, (uint16_t)len);
    return cached;
  }

  return do_constantize(mrb, str, cache, lfu);
}

MRB_API void
mrb_str_constantize_cache_clear(mrb_state *mrb)
{
  mrb_value cache = mrb_gv_get(mrb, MRB_SYM(__str_constantize_cache__));
  if (mrb_hash_p(cache)) mrb_hash_clear(mrb, cache);

  // Rebuild LFU from scratch
  mrb_value lfu_obj = mrb_gv_get(mrb, MRB_SYM(__str_constantize_lfu__));
  if (mrb_data_p(lfu_obj)) {
    struct RClass *lfu_class = mrb_obj_class(mrb, lfu_obj);
    mrb_value new_lfu = mrb_obj_new(mrb, lfu_class, 0, NULL);
    mrb_gv_set(mrb, MRB_SYM(__str_constantize_lfu__), new_lfu);
  }

  mrb_full_gc(mrb);
}

// ============================================================================
// Gem init / final
// ============================================================================

void
mrb_mruby_str_constantize_gem_init(mrb_state *mrb)
{
  ensure_lfu(mrb); // eagerly allocate so GVs are always valid

  mrb_define_method_id(mrb, mrb->string_class,
                       MRB_SYM(constantize),
                       mrb_str_constantize_m,
                       MRB_ARGS_NONE());

  struct RClass *str_mod =
    mrb_define_module_id(mrb, MRB_SYM(StringUtils));

  mrb_define_module_function_id(mrb, str_mod,
                                MRB_SYM(constantize_cache_clear),
                                mrb_str_constantize_cache_clear_m,
                                MRB_ARGS_NONE());
}

void
mrb_mruby_str_constantize_gem_final(mrb_state *mrb)
{
  (void)mrb;
}

MRB_END_DECL