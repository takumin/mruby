require 'rbconfig'

# Comparing an engine against CRuby is a check a build never reaches, the way
# the Unicode tables are: what runs here is what a contributor runs to ask
# whether mruby-regexp still answers the patterns CRuby answers, and what a
# build that pins a CRuby could run to be told rather than to find out.
#
# The corpus and the baseline live with the gem; this only names the script
# and finds a binary to ask.

DIFFTEST_COMPARE = 'mrbgems/mruby-regexp/tools/difftest/compare.rb'

def difftest_mruby
  # Whichever build is there, unless one is named. A baseline describes the
  # build it was taken against, and compare.rb refuses a build that answers
  # by other rules, so naming the wrong one is reported rather than counted.
  bin = ENV['MRUBY'] || Dir["#{MRUBY_ROOT}/build/*/bin/mruby#{RbConfig::CONFIG['EXEEXT']}"].sort.first
  bin or fail 'no mruby binary to ask: run `rake` first, or set MRUBY'
  bin
end

namespace :regexp do
  desc 'compare mruby-regexp against the host CRuby over the pattern corpus'
  task :difftest do
    sh RbConfig.ruby, "#{MRUBY_ROOT}/#{DIFFTEST_COMPARE}", difftest_mruby
  end

  namespace :difftest do
    desc 'record what the two engines answer differently now as the baseline'
    task :update do
      sh RbConfig.ruby, "#{MRUBY_ROOT}/#{DIFFTEST_COMPARE}", difftest_mruby, '--update'
    end
  end
end
