# frozen_string_literal: true
# typed: true

module T::Types
  class TypedSet < TypedEnumerable
    attr_reader :type

    def underlying_class
      Hash
    end

    # overrides Base
    def name
      "T::Set[#{@type.name}]"
    end

    # overrides Base
    def recursively_valid?(obj)
      obj.is_a?(Set) && super
    end

    # overrides Base
    def valid?(obj)
      obj.is_a?(Set)
    end

    def new(*args)
      Set.new(*T.unsafe(args))
    end

    class Untyped < TypedSet
      def initialize
        super(T.untyped)
      end

      def valid?(obj)
        obj.is_a?(Set)
      end
    end
  end
end
