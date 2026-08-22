# Symbols: interning a name that is not there yet grows the symbol table,
# which is a plain allocation, and the name itself is copied into one.
before = :"a symbol that is already there"
syms = []
begin
  300.times { |i| syms << "sym-#{i}-#{i * i}".to_sym }
rescue NoMemoryError
end
raise "an interned symbol came back wrong" unless syms.empty? || syms[0] == :"sym-0-0"
raise "the earlier symbol was disturbed" unless before.to_s == "a symbol that is already there"

begin
  30.times { |i| "meth#{i}".to_sym.to_proc }
rescue NoMemoryError
end
