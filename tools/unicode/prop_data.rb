# The Unicode Character Database, as the character property table generator
# reads it: which general category and which script every codepoint belongs to,
# and every name `\p{...}` may call one of them by.
#
# The two are what a property escape asks about here. A general category is the
# one classification every codepoint has, out of UnicodeData.txt, and a script
# the one Scripts.txt gives it, Unknown where it gives none. What names them is
# PropertyValueAliases.txt, which spells each value short (`Lu`, `Latn`) and
# long (`Uppercase_Letter`, `Latin`) and sometimes further; every spelling is a
# name the escape may use, since that is how CRuby's engine reads them.
#
# The binary properties beyond these two (Math, Dash, Emoji and the rest of
# PropList.txt) are not here: each is a range list of its own and there are
# some ninety of them, which is a table the engine this generates for does not
# carry. The dozen the POSIX brackets already name are the exception, and they
# are not read here either: the bracket table answers them, and the compiler
# answers those names off it before it reaches this one.
#
# Which release the files are, and where they are, is ucd.rb's to say.

require_relative 'ucd'

module Unicode
  class PropData
    MAX_CP = 0x10FFFF

    # The script of a codepoint Scripts.txt lists under none. The file leaves
    # the unassigned and a few assigned characters out, and Unknown is the
    # value the standard gives them; `Zzzz` is its short name.
    UNKNOWN_SCRIPT = 'Unknown'.freeze

    # `Cased_Letter`, the one general category group whose members the value
    # aliases do not spell out in a way this file reads: the others are every
    # category starting with the group's letter, where this is three of the
    # five under `L`. UAX #44 defines it as exactly these.
    CASED_LETTER = %w[Lu Ll Lt].freeze

    # Every assigned codepoint, which is every category but Cn. Onigmo's own
    # name rather than one the database publishes, and CRuby carries it, so it
    # is spelled here beside the categories it is the union of.
    ASSIGNED = 'Assigned'.freeze

    # The names the compiler answers before it reaches this table, off the
    # POSIX bracket types, and which of them are also value aliases here. Each
    # of the three holds exactly what the bracket type holds, so which side
    # answers is not visible in a match; they are dropped so that the table has
    # no name in it the compiler cannot reach.
    #
    # `punct` is not among them, though `[[:punct:]]` and `\p{Punct}` differ:
    # the bracket takes the nine ASCII symbols Onigmo gives it and the property
    # is the category alone. The compiler answers the property name too, from
    # the same type bit with the category's ASCII beside it, so the name stays
    # out of this table with the other two.
    POSIX_NAMES = %w[
      alpha alnum blank cntrl digit graph lower print punct space upper word
      xdigit ascii alphabetic uppercase lowercase whitespace any
    ].freeze

    # How a name is compared: the case, the underscores, the hyphens and the
    # spaces in it are not part of it, so `\p{Uppercase Letter}`,
    # `\p{uppercase_letter}` and `\p{UppercaseLetter}` are one name. The
    # compiler folds the name it reads the same way before searching.
    def self.normalize(name)
      name.downcase.gsub(/[-_ \t]/, '')
    end

    def self.load(dir = nil)
      new(dir || UCD.dir)
    end

    attr_reader :version

    # The concrete general categories, sorted, each at the index that is its
    # bit in a mask.
    attr_reader :categories

    # The scripts, sorted, each at the index the run table holds for it.
    attr_reader :scripts

    # {name => mask}: the general category values a property escape may name,
    # each as the mask of `categories` it covers. A concrete category is one
    # bit, a group the categories it holds.
    attr_reader :gc_masks

    # [[start, category index], ...] and [[start, script index], ...]:
    # the codepoint space cut into runs over which the answer does not change,
    # ascending from U+0000, each run ending where the next begins and the last
    # at U+10FFFF.
    attr_reader :gc_runs, :script_runs

    # [[normalized name, :gc or :script, value name], ...], sorted by name:
    # every spelling of every value, against what it names.
    attr_reader :names

    def initialize(dir)
      @dir = dir
      @version = UCD::VERSION
      UCD.verify(dir)
      read
      compose
    end

    private

    def read
      # Cn is the category of a codepoint UnicodeData.txt lists no line for,
      # and the file writes that name nowhere, so it arrives as the value every
      # codepoint starts at rather than out of a line.
      @gc_of = Array.new(MAX_CP + 1, 'Cn')
      UCD.general_categories(@dir).each { |cp, cat| @gc_of[cp] = cat }
      @categories = @gc_of.uniq.sort

      @script_of = Array.new(MAX_CP + 1, UNKNOWN_SCRIPT)
      UCD.property_ranges(@dir, 'Scripts.txt').each do |name, ranges|
        ranges.each { |r| r.each { |cp| @script_of[cp] = name } }
      end
      @scripts = @script_of.uniq.sort

      @gc_aliases = UCD.value_aliases(@dir, 'gc')
      @script_aliases = UCD.value_aliases(@dir, 'sc')
    end

    def compose
      @gc_runs = runs_of(@gc_of, @categories)
      @script_runs = runs_of(@script_of, @scripts)
      compose_masks
      compose_names
    end

    # The runs of an answer-per-codepoint array, as [start, index] pairs.
    def runs_of(of, values)
      index = {}
      values.each_with_index { |v, i| index[v] = i }
      runs = []
      prev = nil
      (0..MAX_CP).each do |cp|
        next if of[cp] == prev
        runs << [cp, index.fetch(of[cp])]
        prev = of[cp]
      end
      runs
    end

    def compose_masks
      bit = {}
      @categories.each_with_index { |cat, i| bit[cat] = 1 << i }

      @gc_masks = {}
      @categories.each { |cat| @gc_masks[cat] = bit[cat] }
      # A one letter name is every category starting with that letter, which is
      # how the database groups them and what the value aliases say in the
      # comment they close each group's line with.
      @categories.map { |cat| cat[0] }.uniq.each do |letter|
        @gc_masks[letter] = @categories.select { |cat| cat[0] == letter }
                                       .inject(0) { |m, cat| m | bit[cat] }
      end
      @gc_masks['LC'] = CASED_LETTER.inject(0) { |m, cat| m | bit.fetch(cat) }
      @gc_masks[ASSIGNED] = ((1 << @categories.size) - 1) & ~bit.fetch('Cn')
    end

    # The value aliases keyed by the short name, which is what `gc_masks` calls
    # a general category: the file keys a line by its long name and gives the
    # short one first among the aliases.
    def by_short(aliases)
      short = {}
      aliases.each_value { |names| short[names[0]] = names }
      short
    end

    def compose_names
      seen = {}
      @names = []
      add = lambda do |name, kind, value|
        key = self.class.normalize(name)
        taken = seen[key]
        if taken && taken != [kind, value]
          abort "#{name}: names both #{taken.inspect} and #{[kind, value].inspect}"
        end
        next if taken || POSIX_NAMES.include?(key)
        seen[key] = [kind, value]
        @names << [key, kind, value]
      end

      gc_by_short = by_short(@gc_aliases)
      @gc_masks.each_key do |value|
        # `Assigned` is Onigmo's name and the database lists no alias for it.
        ([value] + (gc_by_short[value] || [])).each { |name| add.call(name, :gc, value) }
      end

      @scripts.each do |script|
        aliases = @script_aliases[script] or abort "#{script}: no value alias names it"
        ([script] + aliases).each { |name| add.call(name, :script, script) }
      end

      @names.sort!
    end
  end
end
