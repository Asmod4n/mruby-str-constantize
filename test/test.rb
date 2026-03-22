##
# Tests for mruby-str-constantize
#

module Outer
  module Inner
    class Leaf; end
    CONST_VALUE = 42
  end
  class Node; end
end

class TopLevel; end

assert("String#constantize - top-level class") do
  assert_equal TopLevel, "TopLevel".constantize
end

assert("String#constantize - nested module") do
  assert_equal Outer::Inner, "Outer::Inner".constantize
end

assert("String#constantize - deeply nested class") do
  assert_equal Outer::Inner::Leaf, "Outer::Inner::Leaf".constantize
end

assert("String#constantize - leading :: is accepted") do
  assert_equal TopLevel, "::TopLevel".constantize
end

assert("String#constantize - leading :: with nested path") do
  assert_equal Outer::Inner::Leaf, "::Outer::Inner::Leaf".constantize
end

assert("String#constantize - returns same object on repeated calls (cache hit)") do
  a = "Outer::Inner::Leaf".constantize
  b = "Outer::Inner::Leaf".constantize
  assert_same a, b
end

assert("String#constantize - NameError on unknown constant") do
  assert_raise(NameError) { "NoSuchConstantXyz".constantize }
end

assert("String#constantize - NameError on unknown nested constant") do
  assert_raise(NameError) { "Outer::NoSuchConstantXyz".constantize }
end

assert("String#constantize - NameError on empty string") do
  assert_raise(NameError) { "".constantize }
end

assert("String#constantize - NameError on bare ::") do
  assert_raise(NameError) { "::".constantize }
end

assert("String#constantize - NameError on empty segment (Foo::::Bar)") do
  assert_raise(NameError) { "Outer::::Inner".constantize }
end

assert("String#constantize - TypeError when intermediate segment is not a module") do
  # Outer::Inner::CONST_VALUE is an integer, not a class/module.
  # Traversing through it must raise TypeError.
  assert_raise(TypeError) { "Outer::Inner::CONST_VALUE::Anything".constantize }
end

assert("StringUtils.constantize_cache_clear - clears and re-resolves") do
  first  = "Outer::Inner::Leaf".constantize
  StringUtils.constantize_cache_clear
  second = "Outer::Inner::Leaf".constantize
  assert_equal first, second
end

assert("StringUtils.constantize_cache_clear - returns nil") do
  assert_nil StringUtils.constantize_cache_clear
end

assert("String#constantize - key longer than 64 bytes bypasses LFU but still resolves") do
  # Build a real constant path that exceeds KEY_MAX (64 chars).
  # "Outer::Inner::Leaf" is short; we need the string itself to be >64 bytes.
  # We create a module with a long name at runtime.
  long_name = "A" * 65
  Object.const_set(long_name.to_sym, Module.new)
  result = long_name.constantize
  assert_equal Module, result.class
end

assert("String#constantize - Object class itself") do
  assert_equal Object, "Object".constantize
end

assert("String#constantize - resolves to Module as well as Class") do
  assert_equal Outer::Inner, "Outer::Inner".constantize
  assert_kind_of Module,     "Outer::Inner".constantize
end
