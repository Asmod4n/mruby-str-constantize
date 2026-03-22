MRuby::Gem::Specification.new('mruby-str-constantize') do |spec|
  spec.license  = 'Apache-2'
  spec.author   = 'Hendrik Beskow'
  spec.summary  = 'String#constantize with an LFU cache for mruby'

  spec.add_dependency 'mruby-c-ext-helpers'
end
