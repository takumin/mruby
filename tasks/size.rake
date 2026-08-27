require "json"
require "mruby/source"

# What a build weighs, and what a change did to it.
#
# `rake size` builds, writes `#{build_dir}/size.json` for every target that is
# not internal, and prints what it wrote. `rake "size:diff[before.json,after.json]"`
# reads two of those files and writes the change between them as Markdown,
# which is what `.github/workflows/size.yml` posts on a pull request.
#
# What is measured is the allocated sections a linked file carries -- text,
# data and bss -- and not the size of the file on disk. The default build
# compiles with `-g`, and DWARF is most of what an unstripped file weighs while
# none of it is loaded: the file size of `bin/mruby` is some six times its text,
# and moves with the debug information rather than with the code.
#
# `libmruby.a` is reported as the sum of the objects the archive holds rather
# than measured as a file, because `size` reads an archive as its members and
# an archive carries a symbol table and headers that no build ever loads. The
# objects are reported one by one below that sum, which is where a change is
# read: the sum says the library grew, the object says where.

module MRuby
  module Size
    # What `size.json` holds. A baseline written by another shape of this file
    # is read as no baseline at all rather than as numbers to line up against
    # the ones here.
    SCHEMA = 1

    SECTIONS = %w[text data bss].freeze

    # The comment the workflow keeps on a pull request is found again by this
    # marker, so that a push edits the one comment that is there rather than
    # leaving another below it.
    MARKER = "<!-- mruby-size-report -->".freeze

    # How many object rows the details hold. A change that touches more objects
    # than this is a change to how the whole tree is compiled, and the rows past
    # the first few say nothing the sum has not said already.
    OBJECT_ROWS = 40

    # Berkeley format, which is what `size` prints without `-A`: a header row,
    # then one row per file of `text data bss dec hex filename`. A row is taken
    # for its first three columns and its last, and anything that is not five
    # numbers followed by a name -- the header, and the complaint `size` makes
    # about a file it cannot read as an object -- is not a row.
    ROW = /\A\s*(\d+)\s+(\d+)\s+(\d+)\s+\d+\s+[0-9a-fA-F]+\s+(.+?)\s*\z/

    class << self
      # The command that reports sections. `SIZE` names it for a build whose
      # objects the host's own `size` cannot read; a cross build's binutils are
      # prefixed, and the build config knows the prefix while this file does not.
      def command
        ENV["SIZE"] || "size"
      end

      # `bin/mruby-config` is a shell script and `size` says so on stderr while
      # it reports every other file it was handed. Reading rows rather than an
      # exit status is what lets the script be handed over with the rest and
      # left out of the result.
      def measure(files)
        files = files.select {|f| File.file?(f)}
        return {} if files.empty?

        sizes = {}
        IO.popen([command, *files, err: File::NULL]) do |io|
          io.each_line do |line|
            next unless (m = ROW.match(line))
            sizes[m[4]] = {"text" => m[1].to_i, "data" => m[2].to_i, "bss" => m[3].to_i}
          end
        end
        sizes
      rescue Errno::ENOENT
        abort "#{command}: no such command. Set SIZE to the one that reads this build's objects."
      end

      def total(sizes)
        sizes.each_value.with_object(Hash[SECTIONS.map{|s| [s, 0]}]) do |size, sum|
          SECTIONS.each {|s| sum[s] += size[s]}
        end
      end

      # The first line of what the compiler answers `--version` with, which is
      # the whole of what identifies it. A compiler that has no such option --
      # MSVC -- leaves the field empty rather than failing the report.
      def compiler_version(build)
        IO.popen([build.cc.command, "--version"], err: File::NULL, &:gets).to_s.strip
      rescue Errno::ENOENT, Errno::EACCES
        ""
      end

      def report(build)
        prefix = "#{build.build_dir}/"
        strip = ->(path) { path.start_with?(prefix) ? path[prefix.length..] : path }

        # `build.bins` is what the config asked for by name, and every gem
        # that installs a command asks for it on its own; `tasks/bin.rake`
        # links both into the same `bin` directory.
        bins = build.bins + build.gems.flat_map(&:bins)
        binaries = measure(bins.uniq.sort.map {|bin| build.exefile("#{build.build_dir}/bin/#{bin}")})
        objects = measure(build.libmruby_objs.flatten)

        {
          "schema" => SCHEMA,
          "target" => build.name,
          "config" => File.basename(MRuby::Build.mruby_config_path, ".rb"),
          "revision" => MRuby::Source::MRUBY_FULL_REVISION,
          "environment" => {
            "cc" => build.cc.command,
            "cc_version" => compiler_version(build),
            "platform" => RUBY_PLATFORM,
          },
          "libmruby" => total(objects),
          "binaries" => binaries.transform_keys(&strip).sort.to_h,
          "objects" => objects.transform_keys(&strip).sort.to_h,
        }
      end

      # What `rake size` prints: the same rows the report holds, without the
      # objects, which are thousands of bytes of table that only a comparison
      # has anything to say about.
      def table(report)
        rows = [["libmruby.a", report["libmruby"]]] + report["binaries"].to_a
        width = rows.map {|name, _| name.length}.max
        header = "  %-#{width}s  %12s %12s %12s" % ["", *SECTIONS]
        [
          "#{report["target"]} (#{report["config"]})",
          header,
          *rows.map {|name, size|
            "  %-#{width}s  %12s %12s %12s" % [name, *SECTIONS.map {|s| comma(size[s])}]
          },
        ].join("\n")
      end

      def comma(n)
        n.to_s.reverse.scan(/\d{1,3}/).join(",").reverse
      end

      def delta(n)
        n.zero? ? "±0" : "%+d" % n
      end

      def percent(before, after)
        return "" if before.zero? || before == after
        " (%+.2f%%)" % ((after - before) * 100.0 / before)
      end

      # A cell of the comparison table: what the section weighs now, and what
      # the change was. A row whose artifact is only on one side says so in
      # place of a number, since a number the other side never had is not a
      # change of anything. A report with no baseline compares nothing and
      # says only what the sections weigh.
      def cell(before, after, compare)
        return "—" unless after
        return comma(after) unless compare
        return "#{comma(after)} `new`" unless before
        "#{comma(after)} `#{delta(after - before)}`"
      end

      def changed?(before, after)
        before.nil? || after.nil? || SECTIONS.any? {|s| before[s] != after[s]}
      end

      def rows(before, after)
        (before.keys | after.keys).sort.map {|name| [name, before[name], after[name]]}
      end

      def table_md(title, entries, compare)
        [
          "| #{title} | text | data | bss |",
          "|:--|--:|--:|--:|",
          *entries.map {|name, before, after|
            cells = SECTIONS.map {|s| cell(before && before[s], after && after[s], compare)}
            "| #{name} | #{cells.join(" | ")} |"
          },
        ].join("\n")
      end

      def short(revision)
        revision.to_s[0, 10]
      end

      def headline(before, after)
        return "**No baseline to compare against.**" unless before
        change = SECTIONS.sum {|s| after[s] - before[s]}
        return "**No change in `libmruby`.**" if change.zero?
        base = SECTIONS.sum {|s| before[s]}
        "**`libmruby` #{delta(change)} bytes**#{percent(base, base + change)}"
      end

      # What the two reports were made by, which is what says whether the
      # comparison means anything: a baseline measured by another compiler on
      # another machine differs from this build in ways the change had no part
      # in, and the difference is worth more than the numbers below it.
      def provenance(before, after)
        env = after["environment"]
        line = ["`#{env["cc_version"].empty? ? env["cc"] : env["cc_version"]}`", "`#{env["platform"]}`"]
        line << if before
                  "base `#{short(before["revision"])}` → head `#{short(after["revision"])}`"
                else
                  "head `#{short(after["revision"])}`"
                end
        note = if before && before["environment"] != env
                 "\n\n> [!WARNING]\n> The baseline was measured by " \
                 "`#{before["environment"]["cc_version"]}` on `#{before["environment"]["platform"]}`. " \
                 "Part of every number below is that difference rather than this change."
               else
                 ""
               end
        line.join(" · ") + note
      end

      # Which object the change was in, which is the only thing a comparison
      # has to say that the sum above it has not: with no baseline there is no
      # change to attribute, and the hundred and sixty rows of what every
      # object weighs are worth no one's scrolling.
      def objects_md(before, after)
        return nil unless before

        changed = rows(before["objects"], after["objects"]).select {|_, b, a| changed?(b, a)}
        return "No object of `libmruby` changed." if changed.empty?

        shown = changed.first(OBJECT_ROWS)
        body = table_md("Object", shown.map {|name, b, a| ["`#{name}`", b, a]}, true)
        body += "\n\n…and #{changed.length - shown.length} more." if changed.length > shown.length
        <<~DETAILS.chomp
          <details><summary>Objects that changed (#{changed.length})</summary>

          #{body}

          </details>
        DETAILS
      end

      # The whole comment, from a baseline that may not be there: a pull request
      # opened before `master` ever measured itself, or one whose baseline
      # artifact has aged out of its retention, still says what it weighs.
      def report_md(before, after)
        summary = [["**`libmruby`**", before && before["libmruby"], after["libmruby"]]] +
                  rows(before ? before["binaries"] : {}, after["binaries"])
                    .map {|name, b, a| ["`#{name}`", b, a]}

        [
          MARKER,
          "### Size report",
          headline(before && before["libmruby"], after["libmruby"]),
          provenance(before, after),
          table_md("#{after["target"]} (#{after["config"]})", summary, !before.nil?),
          objects_md(before, after),
        ].compact.join("\n\n") << "\n"
      end

      def read(path)
        return nil if path.to_s.empty? || !File.file?(path)
        report = JSON.parse(File.read(path))
        report["schema"] == SCHEMA ? report : nil
      end
    end
  end
end

desc "report the size of what each target built"
task :size => :all do
  MRuby.each_target do |build|
    next if build.internal?

    report = MRuby::Size.report(build)
    path = "#{build.build_dir}/size.json"
    File.write(path, JSON.pretty_generate(report) << "\n")
    puts
    puts MRuby::Size.table(report)
    puts
    puts "Size report written to #{path.relative_path}"
  end
end

namespace :size do
  # The two files this reads are written by `rake size`, here and on whatever
  # machine measured the baseline. Nothing is built, so a checkout with no
  # build in it can still turn two reports into a comment.
  desc "write the change between two size.json files as Markdown"
  task :diff, [:before, :after, :out] do |_t, args|
    after = MRuby::Size.read(args[:after]) or
      abort "size:diff: #{args[:after].inspect} is not a size report this can read."
    markdown = MRuby::Size.report_md(MRuby::Size.read(args[:before]), after)
    args[:out] ? File.write(args[:out], markdown) : print(markdown)
  end
end
