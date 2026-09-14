module Digest
  # What every digest object answers, whichever algorithm it computes.
  # Digest::Base carries the four methods below for the algorithms this gem
  # defines; a digest written in Ruby includes this module and writes them
  # itself, and the rest of the protocol follows from them.
  module Instance
    def update(str)
      raise NotImplementedError, "#{self.class} does not implement update()"
    end

    def <<(str)
      update(str)
    end

    def reset
      raise NotImplementedError, "#{self.class} does not implement reset()"
    end

    def finish
      raise NotImplementedError, "#{self.class} does not implement finish()"
    end
    private :finish

    def block_length
      raise NotImplementedError, "#{self.class} does not implement block_length()"
    end

    def digest_length
      digest.bytesize
    end

    # call-seq:
    #   digest_obj.digest         -> string
    #   digest_obj.digest(string) -> string
    #
    # The digest as bytes. Given a string, that string's digest is returned
    # and the object is left as empty as it was found; given nothing, it is
    # the digest of what has been fed in so far, and feeding it more carries
    # on from where it stood.
    def digest(*args)
      case args.size
      when 0
        # #finish leaves the object somewhere only a reset carries on from,
        # so what is finished here is a copy. The copy's #finish is reached
        # through __send__ because the method is private, as CRuby's is.
        dup.__send__(:finish)
      when 1
        reset
        update(args[0])
        value = finish
        reset
        value
      else
        raise ArgumentError, "wrong number of arguments (given #{args.size}, expected 0..1)"
      end
    end

    # call-seq:
    #   digest_obj.digest! -> string
    #
    # The digest of what has been fed in so far, leaving the object empty.
    def digest!
      value = finish
      reset
      value
    end

    def hexdigest(*args)
      Digest.hexencode(digest(*args))
    end

    def hexdigest!
      Digest.hexencode(digest!)
    end

    def to_s
      hexdigest
    end

    def length
      digest_length
    end
    alias size length

    # Another digest object is equal where the two have been fed the same
    # bytes; a string is compared against the hexadecimal digest, which is
    # what a digest is usually written down as.
    def ==(other)
      if other.is_a?(Digest::Instance)
        digest == other.digest
      elsif other.is_a?(String)
        to_s == other
      else
        false
      end
    end

    # A digest object of the same class with nothing fed into it.
    def new
      dup.reset
    end

    def inspect
      "#<#{self.class}: #{hexdigest}>"
    end
  end

  class Class
    # call-seq:
    #   Digest::SHA256.digest(string) -> string
    #
    # The digest of the string, without the object having to be kept.
    def self.digest(str, *args)
      new(*args).digest(str)
    end

    # call-seq:
    #   Digest::SHA256.hexdigest(string) -> string
    def self.hexdigest(str, *args)
      Digest.hexencode(digest(str, *args))
    end
  end
end
