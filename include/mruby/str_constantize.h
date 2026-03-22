#pragma once
#include <mruby.h>

MRB_BEGIN_DECL

/*
 * Resolve a Ruby constant name from a String, with an LFU cache.
 *
 * Supports nested names ("Foo::Bar::Baz") and leading "::" for
 * explicit root-anchored lookup.
 *
 * Raises NameError if any segment is not found or is the wrong type.
 * Raises TypeError for non-String input.
 */
MRB_API mrb_value mrb_str_constantize(mrb_state *mrb, mrb_value str);

/*
 * Clear the class-name → constant LFU cache.
 * Call after unloading gems or redefining constants at runtime.
 */
MRB_API void mrb_str_constantize_cache_clear(mrb_state *mrb);

MRB_END_DECL