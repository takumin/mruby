# `mrb_equal()` と Fiber の C 境界　検討の引き継ぎ

この文書は設計検討の記録です。
コードは一行も変えていません。
利用者から「OK と言うまで着手しない」という指示が出ているので、実装に進む前に確認を取ってください。

## 出発点の問い

`mrb_equal()` と `mrb_equal_in_c()` を統合できるか。

検討の途中で受け入れ条件が順に足されました。現時点で有効なものは次の四つです。

1. 構造的に VM を再入できない呼び出し側があること
2. 性能を犠牲にしないこと
3. `mrb_equal()` の API 契約を変えることは視野に入れてよい
4. 移植性が下がる案は全て却下（mruby は組み込みなど特殊な環境で動くため）

途中で目標が広がり、最終的な関心は次に移りました。

> `Fiber.yield` を含む `==` が、どこから呼ばれても通るようにしたい。

## 現在のコードの事実

- `src/object.c:90` `mrb_equal_in_c()`。三状態（`TRUE` / `FALSE` / `-1`）を返す内部関数。`-1` は「`obj1` の `==` が Ruby で書かれているので C では答えられない」。
- `src/object.c:127` `mrb_equal()`。`mrb_equal_in_c()` を呼び、`-1` なら `mrb_funcall_argv()` で `==` を送る。実質3行のラッパで、判定の論理は一箇所にしかない。
- `include/mruby/internal.h:113` `mrb_equal_in_c()` は内部関数。公開 API は `mrb_equal()` だけ。
- `:send` プロトコル。`Array#__svalue_eq`（`src/array.c:2084`）と `Hash#__value_eq`（`mrbgems/mruby-hash-ext/src/hash_ext.c:346`）が `-1` を `:send` に翻訳し、Ruby 側（`mrblib/enum.rb:169`、`mrbgems/mruby-enum-ext/mrblib/enum.rb:252`, `:758`、`mrbgems/mruby-hash-ext/mrblib/hash.rb:236`, `:257`, `:278`, `:299`）が自分の VM で `==` を送り直す。
- この設計の由来はコミット `305d1f1`（`enum.rb: compare an element as mrb_equal() does`）。

## 結論1　統合そのものは割に合わない

二つの関数の差分は、どちらも答えられない場合（Ruby で書かれた `==`）の後始末だけです。
`mrb_equal()` は VM を入れ子にして答え、`mrb_equal_in_c()` は `-1` を返して呼び出し側に送らせます。
消せる重複コードは存在せず、統合で消えるのは名前だけです。

三状態を `mrb_bool` に畳めないので、一本にするなら戻り型か引数を変えることになります。

- `int` を返す形。`if (mrb_equal(...))` が警告もなく通り、`-1` が真になる。静かな破壊。
- `mrb_value` を返す形。`mrb_value` はどのボクシング設定でも構造体（`include/mruby/boxing_nan.h:40` ほか）なので既存の呼び出しは全てコンパイルエラーになる。破壊は音を立てるが、in-tree だけで28箇所（15ファイル）が `mrb_funcall_argv` の後始末を各自で書くことになる。
- 出力引数を足す形。関数は一つになるが、VM を再入できる大多数が永久に `NULL` を書き続ける。

性能上の動機もありません。
熱い経路は `mrb_equal()` を通っておらず、`Array#==` は要素ごとのメソッド探索を避けるために同じ手順を自前で書き下しています（`src/array.c:1984` のコメント）。

**唯一やる価値がある小改善**：`-1` の経路で `mrb_equal_in_c()` が `mrb_method_search_vm()`（`src/object.c:115`）で見つけたメソッドを捨て、`mrb_funcall_argv()` が同じ探索をやり直しています。
本体に「Ruby の `==` を呼んでよいか」の引数を足し、見つけた `mrb_method_t` をその場で起動すれば二重探索は消せます。
公開 API を変えずにできます。

## 結論2　「どこから呼ばれても」は `==` の問題ではない

`Fiber.yield` を弾いているのは `fiber_check_cfunc()`（`mrbgems/mruby-fiber/src/fiber.c:160`）です。
いまの fiber の呼び出し履歴を底まで走査し、`cci > 0` のフレームが一つでもあれば `FiberError` を投げます。
`cci` が 0 でないのは `mrb_funcall` 系が積む `CINFO_DIRECT` と、入れ子の `mrb_vm_run` が積む `CINFO_SKIP` です。
`OP_SEND` が積むフレームは `CINFO_NONE`（`src/vm.c:3722`）なので、C でメソッドを定義していること自体は問題になりません。

成立条件は「fiber の底から `Fiber.yield` までに C のフレームが一つもない」ことで、`==` という特定のメソッドの話ではありません。

### 実測（この木をビルドして確認済み）

```
OK   Enumerable#include?（Ruby 実装）      Array#each / Hash#each / Integer#times のブロック
OK   Kernel#send   instance_eval   Ruby 定義の initialize 経由の Class#new
NG   Array#include? / #index / #delete / #count(arg) / #== / #-
NG   Hash#==   Range#==   Struct#==
NG   Array#sort（<=> 経由）   文字列補間（to_s 経由）   Class.new { }（mrb_yield 経由）
```

境目は実装言語です。
`Array#each` や `Integer#times` が通るのは `mrblib/array.rb:15`、`mrblib/numeric.rb:72` で Ruby で書かれているからです。
`Kernel#send` が通るのは末尾委譲（`mrb_exec_irep()`、`src/vm.c:1635`）でフレームを置き換えているからです。

`==` を全部片付けても性質は成立しません。
`sort` は `<=>`、文字列補間は `to_s`、`uniq` や `-` はキーの比較を通ります。

## 検討した案と評価

| 案 | 完全性 | 移植性 | 性能 | 外部 C の参加 | 判定 |
|---|---|---|---|---|---|
| fiber ごとの C スタック（`swapcontext`） | 満たす | 落ちる | 変わらない | 不要 | **却下**（条件4） |
| C スタックのコピー（`setjmp` + `memcpy`） | 満たす | やや落ちる | 切り替え時のみ | 不要 | **却下**（条件4） |
| fiber を OS スレッドで実装 | 満たす | 落ちる | 切り替えが重い | 不要 | **却下**（条件4） |
| 継続渡し（`lua_callk` 相当） | in-tree のみ | 保てる | 通常経路は変わらない | できる | 検討中 |
| C の速い経路 + Ruby の続きへ末尾委譲 | in-tree のみ | 保てる | 通常経路は変わらない | できない | 検討中 |
| 走査を Ruby 実装へ移す | in-tree のみ | 保てる | 落ちる | できない | 消極的 |
| 再開時に C フレームをやり直す | ー | ー | ー | ー | **不成立**（副作用のある `==` を二度呼ぶ） |

C スタック方式が却下されたので、完全性は諦めることになります。
残る案はどれも「書き換えた分だけ通る」形で、性質が VM ではなく関数ごとのオプトインに宿ります。
外部 gem が `==` を呼べばその経路は破れたままです。

## 継続渡しの設計メモ

### 必要になる API（まだ存在しない）

```c
/* 「ここで Ruby を呼ぶ。答えが出たら k を呼べ」 */
mrb_value mrb_funcall_k(mrb_state *mrb, mrb_value recv, mrb_sym mid,
                        mrb_int argc, const mrb_value *argv,
                        mrb_value (*k)(mrb_state*, mrb_value self, mrb_value result, void *ctx),
                        void *ctx);
```

`fiber_check_cfunc()` の判定は「`cci > 0` なら不可」から「継続を持たない `cci > 0` なら不可」に変わります。

mruby の `Fiber.yield` は context を差し替えて普通に return する作り（`mrb_fiber_yield()` は `fiber_switch_context()` のあと素直に戻る）なので、C のフレームを捨てて fiber の底まで飛ぶ経路を新設することになります。

### 書く側が守る決まり

- `mrb_funcall_k()` は必ず `return` で呼ぶ。yield が起きるとその C フレームは捨てられるので、後ろに書いた処理は実行されない。
- C のローカルに置いていた状態（ループ変数など）を全部 state 構造体へ移す。
- state は `mrb_value` を持つので、中断中も GC から辿れる形（`RData` として確保しフレームに紐づけるなど）で保持する。ここは `mrb_funcall_k()` 側の設計事項。
- Ruby を呼んだ後は `DATA_PTR(self)` や配列の生ポインタを取り直す。
- 後始末を `RData` に任せれば、例外で飛んだときの `free` を書く場所を探さずに済む。

### 書き換え例

```c
struct index_state { mrb_value obj; mrb_int i; };

static mrb_value index_resume(mrb_state *mrb, mrb_value self, mrb_value eq, void *p);

static mrb_value
index_step(mrb_state *mrb, mrb_value self, void *p)
{
  struct index_state *s = p;
  struct mylist *l = DATA_PTR(self);          /* Ruby が走った後なので取り直す */
  while (s->i < l->len) {
    int r = mrb_equal_in_c(mrb, l->items[s->i], s->obj);
    if (r >= 0) {
      if (r) return mrb_int_value(mrb, s->i);
      s->i++;
      continue;
    }
    /* Ruby の == に当たった。ここでフレームを手放す */
    return mrb_funcall_k(mrb, l->items[s->i], MRB_OPSYM(eq), 1, &s->obj, index_resume, s);
  }
  return mrb_nil_value();
}

static mrb_value
index_resume(mrb_state *mrb, mrb_value self, mrb_value eq, void *p)
{
  struct index_state *s = p;
  if (mrb_test(eq)) return mrb_int_value(mrb, s->i);
  s->i++;
  return index_step(mrb, self, s);
}
```

### 外部 gem への影響

何もしない gem はそのまま動きます。
`mrb_funcall()` は残り、コンパイルも挙動も変わりません。
その gem の C の下で `Fiber.yield` を含む Ruby メソッドが呼ばれたときだけ、今までどおり `FiberError` になります。
壊れるのではなく恩恵を受けないだけで、これが「in-tree のみ」の意味です。

### 設計上の制約

再帰検出（`src/kernel.c:189` の `mrb_recursive_method_p()`、`:211` の `mrb_recursive_func_p()`）は callinfo の連なりを `mid` と `stack[0]` / `stack[1]` で走査します。
継続のフレームがこれらを保たないと、`Array#==` や `Hash#==` の自己参照検出が壊れます。
C のローカルではなく callinfo を見る作りなので、フレームの内容さえ引き継げば両立します。

## in-tree 書き換え対象の棚卸し

選別の基準は「C が Ruby を呼び、その戻り値を受けて同じ C フレームで処理を続ける」ことです。
末尾で呼んで返すだけのものは継続を持つ必要がありません。

### 第一層　`==` を C のループで回すもの

| 場所 | 関数 | メソッド | 形 |
|---|---|---|---|
| `src/array.c:1605` | `mrb_ary_index_m` | `Array#index` | 走査 |
| `src/array.c:1647` | `mrb_ary_rindex_m` | `Array#rindex` | 走査 |
| `src/array.c:1989` | `mrb_ary_eq` | `Array#==` | 走査 |
| `src/array.c:2122` | `mrb_ary_delete` | `Array#delete` | 走査 |
| `src/hash.c:2006` | `mrb_hash_has_value` | `Hash#has_value?` | 走査 |
| `src/hash.c:2173` | `mrb_hash_except_keys` | `Hash#except` | 二重走査 |
| `src/hash.c:2327` | `mrb_hash_equal` | `Hash#==` | 走査 |
| `src/range.c:204` | `range_eq` | `Range#==` | 二回呼んで合成 |
| `mrbgems/mruby-struct/src/struct.c:713`, `:747` | `mrb_struct_equal`, `mrb_struct_eql` | `Struct#==`, `#eql?` | 走査 |
| `mrbgems/mruby-data/src/data.c:450` | `mrb_data_equal` | `Data#==` | 走査 |
| `mrbgems/mruby-enum-ext/src/enum.c:68` | `ary_count` | `Array#count(arg)` | 走査 |
| `mrbgems/mruby-array-ext/src/array.c:95`, `:127`, `:1854` | `ary_assoc`, `ary_rassoc`, `ary_include` | `#assoc`, `#rassoc`, `#include?` | 走査 |
| `mrbgems/mruby-hash-ext/src/hash_ext.c:297` | `hash_key_i` | `Hash#key` | コールバック内 |
| `mrbgems/mruby-task/src/task.c:1164` | `mrb_task_s_get` | 名前引き | 走査 |
| `mrbgems/mruby-rational/src/rational.c:685` | `rational_eq_b` | `Rational#==` | 受けて続ける |

17関数。
`hash_key_i` だけは `mrb_hash_foreach()` に渡すコールバックなので、歩く側も一緒に継続渡しにしないと途中で止まれません。

この層は一番軽く済みます。
`mrb_equal_in_c()` が C で答えられる分を先に消化し、Ruby の `==` に当たったときだけ継続に入る形にできるので、通常経路の速度は変わりません。

### 第二層　`<=>` を C から呼ぶもの

| 場所 | 関数 | 備考 |
|---|---|---|
| `src/array.c:2052` | `mrb_ary_cmp` | `Array#<=>` の走査 |
| `src/array.c:2377`, `:2382` | `sort_cmp` | 呼び元は `heapify:2423,2427`、`heap_sort:2453,2469`、`insertion_sort:2489` |
| `src/range.c:43,52,61,67` | | 境界比較 |
| `src/numeric.c:2296,2379,2532` | `cmpnum`, `mrb_cmp` | Comparable の土台 |
| `mrbgems/mruby-enum-ext/src/enum.c:12` | `ary_cmp_ordered` | `min` / `max` |
| `mrbgems/mruby-array-ext/src/array.c:1786` | `ary_cmp_ordered` | |
| `mrbgems/mruby-range-ext/src/range.c:9,60,67,214` | | |

`sort` が最難関です。
比較をヒープ整列と挿入整列のループの内側から呼んでいるので、整列の途中状態（親と子の添字、走査位置）をヒープ上の明示的な状態機械に持ち替えることになります。
整数と文字列に特化した経路（`src/array.c:2169`, `:2220`, `:2295`）は Ruby を呼ばないのでそのまま残せます。

### 第三層　ハッシュキーの `hash`

`src/hash.c:381` の `mrb_obj_hash_code()` が Ruby の `hash` をハッシュ表の内部から呼びます。
呼び元は `h_get` や `h_set` といった表の探索そのもので、`Hash#[]`、`#[]=`、`Set` の全操作、`Array#uniq`、`#-`、`#|` がここを通ります。
表の探索ループ自体を状態機械にすることになり、実装量では `sort` と並びます。

### 第四層　暗黙変換と文字列化

| ヘルパ | 呼ぶ Ruby メソッド | 箇所数 |
|---|---|---|
| `mrb_obj_as_string` | `to_s` | 29 |
| `mrb_ensure_string_type` ほか | `to_str`, `to_ary`, `to_int` | 60超（`src/class.c` 11、`src/vm.c` 8 を含む） |
| `mrb_obj_new` | `initialize` | 31 |

ヘルパを継続渡し版にしても、呼んでいる側が戻り値を受けて処理を続けるので、書き換えは呼び出し元ごとになります。
`src/vm.c` の8箇所が含まれる点が重い。

### 第五層　C からブロックを回すもの

| 場所 | 関数 | メソッド |
|---|---|---|
| `src/class.c:2527` | `mrb_mod_initialize` | `Class.new { }` |
| `mrbgems/mruby-regexp/src/regexp.c:542` | `regexp_match` | `Regexp#match { }` |
| `mrbgems/mruby-regexp/src/regexp.c:2155` | `sub_piece` | `String#gsub { }` |
| `mrbgems/mruby-regexp/src/regexp.c:2979` | `str_scan_m` | `String#scan { }` |

走査系がすでに `mrblib` にあるので、この層は小さい。
`regexp.c` の残る14箇所は `return mrb_funcall_argv(...)` の末尾なので対象外です。

### 対象外

- 末尾で一度呼ぶだけのもの。`src/kernel.c:138`、`src/numeric.c:796,836,1477,1481`、`mrbgems/mruby-process/src/status.c:278`、`mrbgems/mruby-complex/src/complex.c:447,471`
- ツールとテストドライバ（`mruby-bin-mirb`、`mruby-bin-debugger`、`mrbgems/mruby-test/driver.c`）

### 規模

第一層と第二層と第五層で40関数弱。
第三層がハッシュ表の探索一式。
第四層が100箇所超。

## 未決事項

- どこまでの層を対象にするか。第四層まで含めないと `to_s` や `initialize` の中の `Fiber.yield` は通らない。
- `mrb_funcall_k()` の state をどう保持するか（`RData` としてフレームに紐づけるか、別の仕組みか）。
- 継続フレームが `mid` と `stack[0]` / `stack[1]` をどう引き継ぐか（再帰検出との両立）。
- 完全性を諦めることの是非。移植性の条件を守る限り「in-tree では通る」までしか言えない。
- 末尾委譲で置き換えられる `mrb_equal()` の末尾呼び出し（`return mrb_bool_value(mrb_equal(...))`）は、Ruby の `==` が返した値をそのまま返す形になり、`nil` が `false` に畳まれなくなる。挙動差の許容範囲を決める必要がある。

## 進め方の案

1. 第一層だけを入れて `mrb_funcall_k()` の形を確定させる。
2. 第二層の `sort` と第三層のハッシュ表で設計が持つかを確かめる。
3. 第四層は別の判断にする。

独立してやる価値があるのは、結論1に書いた `-1` 経路の二重メソッド探索の除去です。
公開 API を変えず、この検討の結論を待たずに入れられます。

## 再現手順

```sh
gem install rake --no-document
export PATH="$PATH:$(ruby -e 'puts Gem.bindir')"
rake -j8
```

C 境界の地図を取り直すスクリプト。

```ruby
def probe(n); f = Fiber.new { yield }; puts "OK   #{n} -> #{f.resume.inspect}"; rescue => e; puts "NG   #{n} -> #{e.message}"; end

Y = Class.new { def ==(o); Fiber.yield(:eq); true; end
                def <=>(o); Fiber.yield(:cmp); 0; end
                def to_s; Fiber.yield(:to_s); "y"; end }
y = Y.new
o = Object.new

probe("Enumerable#include? (Ruby)") {
  Class.new { include Enumerable; define_method(:each) {|&b| b.call(y) } }.new.include?(o) }
probe("Array#include? (C)")   { [y].include?(o) }
probe("Array#== (C)")         { [y] == [o] }
probe("Hash#== (C)")          { {k: y} == {k: o} }
probe("Struct#== (C)")        { s = Struct.new(:a); s.new(y) == s.new(o) }
probe("Array#sort (<=>)")     { [y, y].sort }
probe("String interpolation") { "#{y}" }
probe("Class.new with block") { Class.new { Fiber.yield(:blk) } }
probe("Kernel#send (末尾委譲)") { y.send(:==, o) }
probe("Array#each (Ruby)")    { [1,2].each { Fiber.yield(:blk) } }
```

```sh
bin/mruby probe.rb
```

## 進行状態

未着手。
利用者の OK 待ち。
