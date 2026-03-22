# mruby-str-constantize

`String#constantize` for [mruby](https://mruby.org/), backed by an LFU cache so repeated lookups cost nothing after the first resolution.

## Usage

```ruby
"Foo".constantize            # => Foo
"Foo::Bar".constantize       # => Foo::Bar
"::Foo::Bar::Baz".constantize # => Foo::Bar::Baz   (root-anchored)
```

Raises `NameError` if the constant does not exist, and `TypeError` if an intermediate segment resolves to something that is not a class or module.

### Cache management

The resolved class is stored in an LFU cache (capacity 128, max key length 64 bytes). To clear it — for example after hot-reloading constants at runtime:

```ruby
StringUtils.constantize_cache_clear
```

### C API

The lookup is also available as a C function for use from other gems:

```c
#include <mruby/str_constantize.h>

mrb_value klass = mrb_str_constantize(mrb, mrb_str_new_lit(mrb, "Foo::Bar"));
mrb_str_constantize_cache_clear(mrb);
```

## Installation

Add the gem to your `build_config.rb`:

```ruby
conf.gem github: 'Asmod4n/mruby-str-constantize', branch: 'main'
```

## Design notes

- **LFU cache** — a hand-rolled, fixed-size (128 entry) Least-Frequently-Used cache with a parallel open-addressing index sits in front of an `mrb_hash`. Cold misses pay the full constant-lookup cost; warm hits are a hash probe plus an LFU touch. Cache hits are 8 times faster on a ryzen 9 5950x.
- **No allocations on the hot path** — cache hits do not allocate.
- **Key length limit** — keys longer than 64 bytes skip the LFU but still resolve correctly via the slow path.
- **GC-safe** — the hash backing the cache is stored in a global variable and is therefore always reachable by the GC.

## License

Apache 2.0 — see [LICENSE](LICENSE).