# Which characters a differential test has to ask about, chosen from the
# Unicode Character Database rather than by hand.
#
# A hand-picked list asks about the characters whoever wrote it thought of. The
# engine does not classify characters one at a time, though: it reads a table
# whose answer is constant over a run and changes at the edges, so what a test
# needs is a character out of every class the tables tell apart. That is a
# selection a rule can make, and this file is the rule.
#
# A representative is the LOWEST codepoint in its class. Nothing about the
# class says which member to take, and the lowest is the one that does not move
# when the class grows: a category or a script gains characters at the top as
# Unicode grows, so picking the lowest keeps the corpus the same corpus across
# a version bump, and the differences a bump does bring stay the ones it really
# brought.
#
# Which release the files are, and where they are, is ucd.rb's to say.

require 'set'
require_relative 'ucd'
require_relative 'ctype_data'

module Unicode
  class CorpusData
    MAX_CP = 0x10FFFF

    # The corpus is compared against whatever CRuby runs it, and that CRuby
    # has a Unicode of its own -- an older one, since a release reaches mruby's
    # pinned tables before it reaches a shipped Ruby. A character assigned
    # after the CRuby's release is unassigned there and classified as nothing,
    # which would read as an engine that disagrees when what disagrees is the
    # two databases.
    #
    # So the corpus stands on characters old enough that every CRuby a
    # contributor plausibly runs already had them. Unicode 13.0 is Ruby 3.0's,
    # and 3.0 is older than any Ruby that builds mruby today.
    #
    # What it costs is the classes that are newer than the floor: a script
    # added in 16.0 has no representative and the corpus does not ask about it.
    # That is the trade -- a question that cannot be answered without asking
    # the two databases to agree first is not one this test can put.
    MAX_AGE = '13.0'.freeze

    def self.load(dir = nil)
      new(dir || UCD.dir)
    end

    attr_reader :version

    # [[codepoint, why], ...] ascending, `why` naming the class it is the
    # representative of, for the generated file to say beside it.
    attr_reader :codepoints

    # The property names to ask each character about: every general category
    # and every group of them, and every script that has a representative.
    #
    # A script whose characters are all newer than the floor is left out, and
    # has to be: the CRuby the corpus is compared against does not know the
    # name, so asking would raise there and answer here, which is the two
    # databases disagreeing rather than the two engines.
    attr_reader :props

    def initialize(dir)
      @dir = dir
      @version = UCD::VERSION
      UCD.verify(dir)
      read
      compose
    end

    private

    def age_rank(age)
      major, minor = age.split('.').map { |n| n.to_i }
      major * 1000 + minor
    end

    def read
      @gc = Array.new(MAX_CP + 1, 'Cn')
      UCD.general_categories(@dir).each { |cp, cat| @gc[cp] = cat }

      @script = Array.new(MAX_CP + 1, 'Unknown')
      UCD.property_ranges(@dir, 'Scripts.txt').each do |name, ranges|
        ranges.each { |r| r.each { |cp| @script[cp] = name } }
      end

      # Old enough for the CRuby the corpus is compared against.
      floor = age_rank(MAX_AGE)
      @old = Array.new(MAX_CP + 1, false)
      UCD.property_ranges(@dir, 'DerivedAge.txt').each do |age, ranges|
        next if age_rank(age) > floor
        ranges.each { |r| r.each { |cp| @old[cp] = true } }
      end

      # What a POSIX bracket and a property escape read, so that a character
      # is picked for each of those classes too and not only for its category.
      @types = CtypeData.load(@dir).types

      @folds = {}
      UCD.each_line(@dir, 'CaseFolding.txt') do |line|
        line = line.sub(/#.*/, '').strip
        next if line.empty?
        code, status, mapping, = line.split(/\s*;\s*/)
        @folds[Integer(code, 16)] = [status, mapping.split(/\s+/).size]
      end
    end

    # The lowest codepoint above ASCII the block says yes to, old enough to be
    # asked about. The block is asked in order and the walk stops at the first
    # yes, so a class whose members start just above ASCII costs a handful of
    # steps; only a class with no old member at all is walked to the end.
    def lowest(from = 0x80)
      cp = from
      while cp <= MAX_CP
        return cp if @old[cp] && yield(cp)
        cp += 1
      end
      nil
    end

    # Whether the sorted, disjoint ranges hold the codepoint.
    def in_ranges?(ranges, cp)
      lo = 0
      hi = ranges.size
      while lo < hi
        mid = lo + (hi - lo) / 2
        if ranges[mid][1] < cp then lo = mid + 1
        elsif ranges[mid][0] > cp then hi = mid
        else return true
        end
      end
      false
    end

    def compose
      picked = {}
      take = lambda do |cp, why|
        next unless cp
        picked[cp] ||= why
      end

      # ASCII whole. It is 128 characters, it is what almost every pattern is
      # about, and every one of them is as old as the database.
      (0..0x7f).each { |cp| take.call(cp, "ascii") }

      # One character of every general category, and of every script. Both are
      # a value per codepoint, so one walk finds the lowest of every class at
      # once rather than one walk a class.
      first_gc = {}
      first_sc = {}
      cp = 0x80
      while cp <= MAX_CP
        if @old[cp]
          first_gc[@gc[cp]] ||= cp
          first_sc[@script[cp]] ||= cp
        end
        cp += 1
      end
      first_gc.keys.sort.each { |cat| take.call(first_gc[cat], "gc=#{cat}") }
      first_sc.keys.sort.each { |name| take.call(first_sc[name], "sc=#{name}") }

      # The categories are every category there is, groups included, since a
      # group is a name for categories that are all old whatever the release.
      # The scripts are the ones a character was found for.
      groups = first_gc.keys.map { |cat| cat[0] }.uniq
      @props = (first_gc.keys + groups + %w[LC Any Assigned] +
                first_sc.keys).sort

      # One in each POSIX type above ASCII, and one outside it: a bracket that
      # stopped holding what it holds and one that started holding what it does
      # not are different mistakes, and each wants a character to show it.
      @types.each do |name, ranges|
        take.call(lowest { |c| in_ranges?(ranges, c) }, name)
        take.call(lowest { |c| !in_ranges?(ranges, c) }, "not #{name}")
      end

      # The shapes case folding comes in, which is what /i reads: a character
      # that folds to one other, one whose folding expands into several, and
      # one that folds to nothing. The two whose folding lands in ASCII are
      # named rather than searched for, being the pair every build carries.
      take.call(lowest { |c| @folds[c] && @folds[c][0] == 'C' }, "folds to one")
      take.call(lowest { |c| @folds[c] && @folds[c][0] == 'F' }, "folding expands")
      take.call(lowest { |c| !@folds.key?(c) && @gc[c] != 'Cn' }, "folds to nothing")
      take.call(0x17f, "folds to ASCII")
      take.call(0x212a, "folds to ASCII")

      # Where the encoding changes width, and where the codepoint space ends.
      # A table walked by codepoint and a string walked by byte meet here.
      [0x7f, 0x80, 0x7ff, 0x800, 0xffff, 0x10000, 0xd7ff, 0xe000,
       0x10fffd, 0x10ffff].each { |cp| take.call(cp, "boundary") }

      @codepoints = picked.keys.sort.map { |cp| [cp, picked[cp]] }
    end
  end
end
