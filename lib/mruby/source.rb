require "pathname"

module MRuby
  module Source
    # mruby's source root directory
    ROOT = Pathname.new(File.expand_path('../../../',__FILE__))

    # Reads a constant defined at version.h
    MRUBY_READ_VERSION_CONSTANT = Proc.new do |name|
      ROOT.join('include','mruby','version.h').read.match(/^#define #{name} +"?([\w\. ]+)"?\r?$/)[1]
    end

    MRUBY_RUBY_VERSION = MRUBY_READ_VERSION_CONSTANT['MRUBY_RUBY_VERSION']
    MRUBY_RUBY_ENGINE = MRUBY_READ_VERSION_CONSTANT['MRUBY_RUBY_ENGINE']

    MRUBY_RELEASE_MAJOR = Integer(MRUBY_READ_VERSION_CONSTANT['MRUBY_RELEASE_MAJOR'])
    MRUBY_RELEASE_MINOR = Integer(MRUBY_READ_VERSION_CONSTANT['MRUBY_RELEASE_MINOR'])
    MRUBY_RELEASE_TEENY = Integer(MRUBY_READ_VERSION_CONSTANT['MRUBY_RELEASE_TEENY'])

    MRUBY_VERSION = [MRUBY_RELEASE_MAJOR,MRUBY_RELEASE_MINOR,MRUBY_RELEASE_TEENY].join('.')
    MRUBY_RELEASE_NO = (MRUBY_RELEASE_MAJOR * 100 * 100 + MRUBY_RELEASE_MINOR * 100 + MRUBY_RELEASE_TEENY)

    MRUBY_RELEASE_YEAR = Integer(MRUBY_READ_VERSION_CONSTANT['MRUBY_RELEASE_YEAR'])
    MRUBY_RELEASE_MONTH = Integer(MRUBY_READ_VERSION_CONSTANT['MRUBY_RELEASE_MONTH'])
    MRUBY_RELEASE_DAY = Integer(MRUBY_READ_VERSION_CONSTANT['MRUBY_RELEASE_DAY'])
    MRUBY_RELEASE_DATE = [MRUBY_RELEASE_YEAR,MRUBY_RELEASE_MONTH,MRUBY_RELEASE_DAY].join('.')

    # The revision the source tree sits at, as the abbreviated commit hash of
    # the git repository holding it. Empty where there is none to ask: an
    # unpacked release archive carries no repository, a tree copied out of one
    # carries no history, and git itself may not be installed.
    MRUBY_REVISION = begin
      if ROOT.join('.git').exist?
        # An argument list rather than a command line, so that a path with a
        # space in it needs no quoting of its own; `git` answers on stderr
        # where it cannot read a revision, and the empty answer is what we
        # report instead.
        rev = IO.popen(['git', '-C', ROOT.to_s, 'rev-parse', '--short=10', '--verify', 'HEAD'],
                       err: File::NULL, &:read)
        $?.success? ? rev.strip : ""
      else
        ""
      end
    rescue SystemCallError
      ""
    end

    MRUBY_BIRTH_YEAR = Integer(MRUBY_READ_VERSION_CONSTANT['MRUBY_BIRTH_YEAR'])

    MRUBY_AUTHOR = MRUBY_READ_VERSION_CONSTANT['MRUBY_AUTHOR']
  end
end
